#define _GNU_SOURCE
#include <sched.h>

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <poll.h>
#include <linux/rpmsg.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdarg.h>
#include <time.h>

#define MAX_DATA_LENGTH 256

#define DEVICE_CORE_START 0x0001U
#define DEVICE_CORE_STOP 0x0002U
#define DEVICE_CORE_CHECK 0x0003U
#define DEVICE_CORE_SENSOR 0x0004U
#define DEVICE_CORE_HEARTBEAT 0x0005U
#define HEARTBEAT_PING_PREFIX "PING:"
#define HEARTBEAT_PONG_PREFIX "PONG:"
#define HEARTBEAT_STARTUP_GRACE_SECONDS 5
#define HEARTBEAT_PERIOD_MS 500
#define HEARTBEAT_REPLY_TIMEOUT_MS 80
#define HEARTBEAT_MAX_MISSES 5
#define HEARTBEAT_DROP_MARKER "/run/amp_drop_heartbeat"

typedef struct __attribute__((packed))
{
    uint32_t command;
    uint16_t length;
    char data[MAX_DATA_LENGTH];
} data_packet;

/* brain_thread 和 heartbeat_thread 共用同一个 RPMsg fd，发送时必须串行。 */
static pthread_mutex_t rpmsg_write_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t heartbeat_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t heartbeat_reply_cond = PTHREAD_COND_INITIALIZER;
static unsigned int heartbeat_waiting_sequence;
static int heartbeat_reply_received;

static ssize_t write_full(int fd, void *buf, size_t count)
{
    ssize_t ret = 0;
    ssize_t total = 0;

    while (count)
    {
        ret = write(fd, buf, count);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        count -= ret;
        buf += ret;
        total += ret;
    }
    return total;
}

static ssize_t read_full(int fd, void *buf, size_t count)
{
    ssize_t res;
    do
    {
        res = read(fd, buf, count);
    } while (res < 0 && errno == EINTR);
    if ((res < 0 && errno == EAGAIN))
        return 0;
    if (res < 0)
        return -1;
    return res;
}

// 通过UDP发送字符串
void send_str_to_pc(int udp_fd, struct sockaddr_in *pc_addr, const char *fmt, ...)
{
    if (udp_fd < 0 || pc_addr == NULL)
        return;

    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    sendto(udp_fd, buf, strlen(buf), 0, (struct sockaddr *)pc_addr, sizeof(struct sockaddr_in));
    fflush(stdout);
}

// 通过 RPMsg 发送字符串到裸机
void send_str_to_baremetal(int rpmsg_fd, const char *fmt, ...)
{
    if (rpmsg_fd < 0)
        return;

    data_packet tx_data;
    tx_data.command = DEVICE_CORE_CHECK;

    va_list args;
    va_start(args, fmt);
    tx_data.length = vsnprintf(tx_data.data, MAX_DATA_LENGTH, fmt, args);
    va_end(args);

    pthread_mutex_lock(&rpmsg_write_lock);
    write_full(rpmsg_fd, &tx_data, 6 + tx_data.length);
    pthread_mutex_unlock(&rpmsg_write_lock);
    printf("=> [Linux->裸机] 下发指令: %s\n", tx_data.data);
}

static void add_milliseconds(struct timespec *value, long milliseconds)
{
    value->tv_sec += milliseconds / 1000;
    value->tv_nsec += (milliseconds % 1000) * 1000000L;
    if (value->tv_nsec >= 1000000000L)
    {
        value->tv_sec++;
        value->tv_nsec -= 1000000000L;
    }
}

static int wait_for_heartbeat_reply(unsigned int sequence)
{
    struct timespec deadline;
    int received;

    clock_gettime(CLOCK_REALTIME, &deadline);
    add_milliseconds(&deadline, HEARTBEAT_REPLY_TIMEOUT_MS);

    pthread_mutex_lock(&heartbeat_state_lock);
    while (!heartbeat_reply_received || heartbeat_waiting_sequence != sequence)
    {
        int wait_ret = pthread_cond_timedwait(&heartbeat_reply_cond,
                                              &heartbeat_state_lock,
                                              &deadline);
        if (wait_ret == ETIMEDOUT)
            break;
    }
    received = heartbeat_reply_received &&
               heartbeat_waiting_sequence == sequence;
    pthread_mutex_unlock(&heartbeat_state_lock);
    return received;
}

static void record_heartbeat_reply(unsigned int sequence)
{
    /* 测试开关：保留通信收包，只让心跳状态机忽略 PONG。 */
    if (access(HEARTBEAT_DROP_MARKER, F_OK) == 0)
    {
        return;
    }

    pthread_mutex_lock(&heartbeat_state_lock);
    if (heartbeat_waiting_sequence == sequence)
    {
        heartbeat_reply_received = 1;
        pthread_cond_signal(&heartbeat_reply_cond);
    }
    pthread_mutex_unlock(&heartbeat_state_lock);
}

// Linux 主核快速发送心跳，只在无回信时告警
void *heartbeat_thread(void *arg)
{
    int rpmsg_fd = *(int *)arg;
    unsigned int sequence = 1;
    int consecutive_misses = 0;
    int fault_reported = 0;
    struct timespec next_send;

    /* 裸机启动时需要初始化外设和 RPMsg，宽限期内不进行故障计数。 */
    sleep(HEARTBEAT_STARTUP_GRACE_SECONDS);
    clock_gettime(CLOCK_MONOTONIC, &next_send);

    while (1)
    {
        data_packet heartbeat;
        ssize_t write_ret;
        int reply_received;

        heartbeat.command = DEVICE_CORE_HEARTBEAT;
        heartbeat.length = snprintf(heartbeat.data, MAX_DATA_LENGTH,
                                    HEARTBEAT_PING_PREFIX "%u", sequence);

        pthread_mutex_lock(&heartbeat_state_lock);
        heartbeat_waiting_sequence = sequence;
        heartbeat_reply_received = 0;
        pthread_mutex_unlock(&heartbeat_state_lock);

        pthread_mutex_lock(&rpmsg_write_lock);
        write_ret = write_full(rpmsg_fd, &heartbeat, 6 + heartbeat.length);
        pthread_mutex_unlock(&rpmsg_write_lock);
        reply_received = wait_for_heartbeat_reply(sequence);

        if (write_ret == 6 + heartbeat.length &&
            reply_received)
        {
            consecutive_misses = 0;
            fault_reported = 0;
        }
        else
        {
            if (consecutive_misses < HEARTBEAT_MAX_MISSES)
            {
                consecutive_misses++;
            }
            if (consecutive_misses == 1)
            {
                fprintf(stderr,
                        "[心跳警告] 未收到裸机回信，开始重试（最多 %d 次）。\n",
                        HEARTBEAT_MAX_MISSES);
            }

            if (consecutive_misses >= HEARTBEAT_MAX_MISSES &&
                !fault_reported)
            {
                fprintf(stderr,
                        "[裸机故障] 裸机连续无响应\n");
                fault_reported = 1;
            }
        }

        sequence++;
        add_milliseconds(&next_send, HEARTBEAT_PERIOD_MS);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                               &next_send, NULL) == EINTR)
        {
        }
    }

    return NULL;
}

// 接收裸机数据并上传至Web网页
void *ear_thread(void *arg)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    int rpmsg_fd = *(int *)arg;
    struct pollfd fds;
    data_packet test_data_r;
    char buff_r[MAX_DATA_LENGTH];

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in pc_addr;

    if (udp_fd < 0)
    {
        perror("Socket creation failed");
    }
    else
    {
        memset(&pc_addr, 0, sizeof(pc_addr));
        pc_addr.sin_family = AF_INET;
        pc_addr.sin_port = htons(9998);
        inet_pton(AF_INET, "127.0.0.1", &pc_addr.sin_addr);
        printf("UDP Transmitter Ready -> 127.0.0.1:9998\n");
    }

    memset(&fds, 0, sizeof(struct pollfd));
    fds.fd = rpmsg_fd;
    fds.events |= POLLIN;

    printf("Entering Linux async communication loop...\n");
    while (1)
    {
        int ret = poll(&fds, 1, 500);
        if (ret > 0 && (fds.revents & POLLIN))
        {
            memset(&test_data_r, 0, sizeof(data_packet));
            int read_ret = read_full(rpmsg_fd, &test_data_r, sizeof(data_packet));

            if (read_ret > 0)
            {
                int data_len = test_data_r.length;
                if (data_len > MAX_DATA_LENGTH - 1)
                    data_len = MAX_DATA_LENGTH - 1;
                memcpy(buff_r, test_data_r.data, data_len);
                buff_r[data_len] = '\0';

                if (test_data_r.command == DEVICE_CORE_HEARTBEAT)
                {
                    unsigned int sequence;
                    char trailing;

                    if (sscanf(buff_r, HEARTBEAT_PONG_PREFIX "%u%c",
                               &sequence, &trailing) == 1)
                    {
                        record_heartbeat_reply(sequence);
                    }
                }
                else if (test_data_r.command == DEVICE_CORE_SENSOR)
                {
                    if (strncmp(buff_r, "R:", 2) == 0)
                    {
                        float l_roll = 0.0, l_pitch = 0.0, l_yaw = 0.0;
                        if (sscanf(buff_r, "R:%f,P:%f,Y:%f", &l_roll, &l_pitch, &l_yaw) == 3)
                        {
                            send_str_to_pc(udp_fd, &pc_addr,
                                           "{\"roll\": %.2f, \"pitch\": %.2f, \"yaw\": %.2f, \"error\": 0}",
                                           l_roll, l_pitch, l_yaw);
                        }
                    }
                    else if (strncmp(buff_r, "SONAR:", 6) == 0)
                    {
                        int s_front = 0, s_left = 0, s_right = 0;
                        if (sscanf(buff_r, "SONAR:%d,%d,%d", &s_front, &s_left, &s_right) == 3)
                        {
                            send_str_to_pc(udp_fd, &pc_addr,
                                           "{\"sonar_front\": %d, \"sonar_left\": %d, \"sonar_right\": %d}",
                                           s_front, s_left, s_right);
                        }
                    }
                    else if (strncmp(buff_r, "FAULT:SONAR:", 12) == 0)
                    {
                        unsigned int mask = 0U;
                        if (sscanf(buff_r, "FAULT:SONAR:%u", &mask) == 1)
                        {
                            send_str_to_pc(
                                udp_fd,
                                &pc_addr,
                                "{\"sonar_fault\": true, \"sonar_fault_mask\": %u}",
                                mask);
                        }
                    }
                    else if (strcmp(buff_r, "RECOVER:SONAR") == 0)
                    {
                        send_str_to_pc(
                            udp_fd,
                            &pc_addr,
                            "{\"sonar_fault\": false, \"sonar_fault_mask\": 0}");
                    }
                    else if (strncmp(buff_r, "FAULT:NAV:", 10) == 0)
                    {
                        send_str_to_pc(
                            udp_fd,
                            &pc_addr,
                            "{\"nav_fault\": \"%s\"}",
                            buff_r);
                    }
                }
                else if (test_data_r.command == DEVICE_CORE_CHECK)
                {
                    send_str_to_pc(
                        udp_fd,
                        &pc_addr,
                        "{\"control_ack\": \"%s\"}",
                        buff_r);
                }
                else
                {
                    printf("<= [BareMetal Status] CMD: 0x%04X, DATA: %s\n",
                           test_data_r.command, buff_r);
                }
            }
        }
        else if (ret < 0 && errno != EINTR)
        {
            perror("poll failed");
            break;
        }
    }

    if (udp_fd >= 0)
        close(udp_fd);
    return NULL;
}

// 接收网页指令并下发至裸机
void *brain_thread(void *arg)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    int rpmsg_fd = *(int *)arg;
    int udp_rx_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in rx_addr;
    memset(&rx_addr, 0, sizeof(rx_addr));
    rx_addr.sin_family = AF_INET;
    rx_addr.sin_port = htons(9001);
    rx_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(udp_rx_fd, (struct sockaddr *)&rx_addr, sizeof(rx_addr)) < 0)
    {
        perror("Brain thread bind failed");
        return NULL;
    }

    printf("[Brain Thread] Waiting for PC Commands on Port 9001...\n");

    char recv_buf[256];
    while (1)
    {
        int n = recvfrom(udp_rx_fd, recv_buf, sizeof(recv_buf) - 1, 0, NULL, NULL);
        if (n > 0)
        {
            recv_buf[n] = '\0';
            printf("\n<= [PC->Linux] 收到指令: %s\n", recv_buf);
            send_str_to_baremetal(rpmsg_fd, "%s", recv_buf);
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int ctrl_fd, rpmsg_fd, ret;
    struct rpmsg_endpoint_info eptinfo;
    data_packet test_data;
    ctrl_fd = open("/dev/rpmsg_ctrl0", O_RDWR);
    if (ctrl_fd < 0)
    {
        perror("open rpmsg_ctrl0 failed.\n");
        return -1;
    }

    memcpy(eptinfo.name, "xxxx", 32);
    eptinfo.src = 0;
    eptinfo.dst = 0;
    ret = ioctl(ctrl_fd, RPMSG_CREATE_EPT_IOCTL, eptinfo);
    if (ret != 0)
    {
        perror("ioctl RPMSG_CREATE_EPT_IOCTL failed.\n");
        close(ctrl_fd);
        return -1;
    }

    rpmsg_fd = open("/dev/rpmsg0", O_RDWR);
    if (rpmsg_fd < 0)
    {
        perror("open rpmsg0 failed.\n");
        close(ctrl_fd);
        return -1;
    }

    if (argc > 2 && strcmp(argv[1], "stop") == 0)
    {
        if (strcmp(argv[2], "0") == 0)
        {
            test_data.command = DEVICE_CORE_STOP;
            test_data.length = 1;
            ret = write_full(rpmsg_fd, &test_data, sizeof(data_packet));
            if (ret < 0)
                perror("write_full failed.\n");
        }
        close(rpmsg_fd);
        close(ctrl_fd);
        return 0;
    }

    test_data.command = DEVICE_CORE_CHECK;
    test_data.length = snprintf(test_data.data, MAX_DATA_LENGTH, "WakeUp");
    write_full(rpmsg_fd, &test_data, 6 + test_data.length);

    pthread_t tid_ear, tid_brain, tid_heartbeat;
    pthread_create(&tid_ear, NULL, ear_thread, &rpmsg_fd);
    pthread_create(&tid_brain, NULL, brain_thread, &rpmsg_fd);
    pthread_create(&tid_heartbeat, NULL, heartbeat_thread, &rpmsg_fd);

    pthread_join(tid_ear, NULL);
    pthread_join(tid_brain, NULL);
    pthread_join(tid_heartbeat, NULL);

    close(rpmsg_fd);
    close(ctrl_fd);
    return 0;
}
