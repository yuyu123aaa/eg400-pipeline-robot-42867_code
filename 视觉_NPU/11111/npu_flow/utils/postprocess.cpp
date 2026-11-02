#include "postprocess.hpp"
#include "armnnUtils/DataLayoutIndexed.hpp"
#include "logger_forward.hpp"
#include <opencv2/opencv.hpp>
#include <opencv2/imgcodecs.hpp>
#include <utility>
struct Object {
    cv::Rect_<float> rect;
    int label;
    float prob;
    std::vector<std::vector<float>> kpt;
};

static inline float intersection_area(const Object& a, const Object& b)
{
    cv::Rect_<float> inter = a.rect & b.rect;
    return inter.area();
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects, int left, int right)
{
    int i = left;
    int j = right;
    float p = faceobjects[(left + right) / 2].prob;

    while (i <= j)
    {
        while (faceobjects[i].prob > p)
            i++;

        while (faceobjects[j].prob < p)
            j--;

        if (i <= j)
        {
            // swap
            std::swap(faceobjects[i], faceobjects[j]);

            i++;
            j--;
        }
    }

// #pragma omp parallel sections
    {
// #pragma omp section
        {
            if (left < j) qsort_descent_inplace(faceobjects, left, j);
        }
// #pragma omp section
        {
            if (i < right) qsort_descent_inplace(faceobjects, i, right);
        }
    }
}

static void qsort_descent_inplace(std::vector<Object>& faceobjects)
{
    if (faceobjects.empty())
        return;

    qsort_descent_inplace(faceobjects, 0, faceobjects.size() - 1);
}

static void nms_sorted_bboxes(const std::vector<Object>& faceobjects, std::vector<int>& picked, float nms_threshold)
{
    picked.clear();

    const int n = faceobjects.size();

    std::vector<float> areas(n);
    for (int i = 0; i < n; i++)
    {
        areas[i] = faceobjects[i].rect.area();
    }

    for (int i = 0; i < n; i++)
    {
        const Object& a = faceobjects[i];

        int keep = 1;
        for (int j = 0; j < (int)picked.size(); j++)
        {
            const Object& b = faceobjects[picked[j]];

            // intersection over union
            float inter_area = intersection_area(a, b);
            float union_area = areas[i] + areas[picked[j]] - inter_area;
            // float IoU = inter_area / union_area
            if (inter_area / union_area > nms_threshold) {
                keep = 0;
                break;
            }
        }

        if (keep)
            picked.push_back(i);
    }
}

struct FastDecodeOutputYoloWorkLoad
{
    std::vector<Object> proposals_;
    std::vector<std::vector<float>> sigmoid_tab_;
    std::vector<std::vector<float>> float_tab_;
    std::unordered_map<int, std::vector<int>> stride_anchor_map_;
    int input_h_;
    int input_w_;
    int cls_num_;
    float prob_threshold_;
    float nms_threshold_;
    int head_num_;
    armnnUtils::DataLayoutIndexed data_layout_;

    FastDecodeOutputYoloWorkLoad(): data_layout_(armnnUtils::DataLayoutIndexed(armnn::DataLayout::NCHW)) {}
    ~FastDecodeOutputYoloWorkLoad() {}

    template <typename T>
    void init(const armnn::OutputTensors& outputTensors, int input_h, int input_w, int cls_num, float prob_threshold,  \
                float nms_threshold, const std::unordered_map<int, std::vector<int>>& stride_anchor_map)
    {
        input_h_ = input_h;
        input_w_ = input_w;
        cls_num_ = cls_num;
        prob_threshold_ = prob_threshold;
        nms_threshold_ = nms_threshold;
        stride_anchor_map_ = stride_anchor_map;
        head_num_ = outputTensors.size();
        if (head_num_ != (int)(stride_anchor_map.size())) {
            LOG_ERROR("the number of yolo head {} doesn't equal to the size of strideAnchorMap {}", head_num_, stride_anchor_map.size());
            throw std::runtime_error("the number of yolo head doesn't equal to the size of strideAnchorMap");
        }
        sigmoid_tab_.resize(head_num_, std::vector<float>(256));
        float_tab_.resize(head_num_, std::vector<float>(256));
        buildTable<T>(outputTensors);
    }

    template <typename T>
    std::vector<DetectObject> work(const armnn::OutputTensors& outputTensors)
    {
        proposals_.clear();
        // same as generate_proposals
        for (int i = 0; i < head_num_; i++)
        {
            auto& out_tensor = outputTensors[i].second;
            int stride = input_w_ / out_tensor.GetShape()[data_layout_.GetWidthIndex()];
            if (stride_anchor_map_.find(stride) == stride_anchor_map_.end()) {
                LOG_ERROR("fail to find the stride: {} in stride anchor map", stride);
                throw std::runtime_error("fail to find the stride in stride anchor map\n");
            }
            std::vector<Object> objects_tmp;
            T* feat_data = (T*)out_tensor.GetMemoryArea();
            int feat_w = input_w_ / stride;
            int feat_h = input_h_ / stride;
            int anchor_num = stride_anchor_map_[stride].size() / 2;
            for (int h = 0; h <= feat_h - 1; h++)
            {
                for (int w = 0; w <= feat_w - 1; w++)
                {
                    for (int anchor = 0; anchor <= anchor_num - 1; anchor++)
                    {
                        int class_index = 0;
                        T class_score = std::numeric_limits<T>::lowest();
                        int channel_size = feat_h * feat_w;
                        for (int s = 0; s <= cls_num_ - 1; s++)
                        {
                            int score_index = anchor * (cls_num_ + 5) * channel_size + feat_w * h + w + (s + 5) * channel_size;
                            T score = feat_data[score_index];
                            if (score > class_score)
                            {
                                class_index = s;
                                class_score = score;
                            }
                        }
                        T box_score = feat_data[anchor * (cls_num_ + 5) * channel_size + feat_w * h + w + 4 * channel_size];
                        float final_score = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&box_score)) * sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&class_score));
                        if (final_score >= prob_threshold_)
                        {
                            int dx_index = anchor * (cls_num_ + 5) * channel_size + feat_w * h + w + 0 * channel_size;
                            int dy_index = anchor * (cls_num_ + 5) * channel_size + feat_w * h + w + 1 * channel_size;
                            int dw_index = anchor * (cls_num_ + 5) * channel_size + feat_w * h + w + 2 * channel_size;
                            int dh_index = anchor * (cls_num_ + 5) * channel_size + feat_w * h + w + 3 * channel_size;

                            T dx_tmp = feat_data[dx_index];
                            T dy_tmp = feat_data[dy_index];
                            T dw_tmp = feat_data[dw_index];
                            T dh_tmp = feat_data[dh_index];

                            float dx = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dx_tmp));
                            float dy = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dy_tmp));

                            // float dw = feat[dw_index];
                            // float dh = feat[dh_index];
                            float dw = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dw_tmp));
                            float dh = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dh_tmp));

                            float anchor_w = stride_anchor_map_[stride][anchor * 2 + 0];
                            float anchor_h = stride_anchor_map_[stride][anchor * 2 + 1];

                            // float pred_x = (w + dx) * stride;
                            // float pred_y = (h + dy) * stride;
                            float pred_x = (dx * 2.0f - 0.5f + w) * stride;
                            float pred_y = (dy * 2.0f - 0.5f + h) * stride;
                            // float pred_w = exp(dw) * anchor_w;
                            // float pred_h = exp(dh) * anchor_h;
                            float pred_w = dw * dw * 4.0f * anchor_w;
                            float pred_h = dh * dh * 4.0f * anchor_h;

                            // float x0 = (pred_x - pred_w * 0.5f);
                            // float y0 = (pred_y - pred_h * 0.5f);
                            // float x1 = (pred_x + pred_w * 0.5f);
                            // float y1 = (pred_y + pred_h * 0.5f);

                            Object obj;
                            obj.rect.x = pred_x;
                            obj.rect.y = pred_y;
                            obj.rect.width = pred_w;
                            obj.rect.height = pred_h;
                            obj.label = class_index;
                            obj.prob = final_score;
                            proposals_.push_back(obj);
                        }
                    }
                }
            }
        }
        // std::sort(proposals.begin(), proposals.end(), [](Object& a, Object& b) { return a.prob > b.prob; });
        qsort_descent_inplace(proposals_);
        std::vector<int> picked;
        nms_sorted_bboxes(proposals_, picked, nms_threshold_);

        int count = picked.size();
        // fprintf(stderr, "detection num: %d\n", count);
        std::vector<DetectObject> objects;
        objects.resize(count);
        LOG_VERBOSE("detect item num: {}\n", count);
        for (int i = 0; i < count; i++)
        {
            objects[i].prob = proposals_[picked[i]].prob;
            objects[i].label = proposals_[picked[i]].label;
            objects[i].x = proposals_[picked[i]].rect.x;
            objects[i].y = proposals_[picked[i]].rect.y;
            objects[i].w = proposals_[picked[i]].rect.width;
            objects[i].h = proposals_[picked[i]].rect.height;
            LOG_VERBOSE("detect item {}:  x_center: {}, y_center: {}, w: {}, h: {}, prob: {}, cls: {}\n", i, objects[i].x, objects[i].y, objects[i].w, objects[i].h, objects[i].prob, objects[i].label);
        }
        return objects;
    }
    static inline float sigmoid(float x)
    {
        return static_cast<float>(1.f / (1.f + exp(-x)));
    }
    template <typename T>
    void buildTable(const armnn::OutputTensors& outputTensors)
    {
        for (int x = 0; x < head_num_; x++)
        {
            armnn::Tensor Tensor0 = outputTensors[x].second;
            {
                float scale = Tensor0.GetInfo().GetQuantizationScale();
                int zero_point = Tensor0.GetInfo().GetQuantizationOffset();
                for (int i = std::numeric_limits<T>::lowest(); i <= std::numeric_limits<T>::max(); i++)
                {
                    sigmoid_tab_[x][*static_cast<uint8_t*>((void*)&i)] = sigmoid((float(i) - zero_point) * scale);
                    float_tab_[x][*static_cast<uint8_t *>((void *)&i)] = ((float(i) - zero_point) * scale);
                }
            }
        }
    }
};


std::function<std::vector<DetectObject>(const armnn::OutputTensors& outputTensors)> \
    getFastDecodeOutputYoloWorkLoad(const armnn::OutputTensors& outputTensors, int input_h, int input_w, int cls_num, float prob_threshold, \
            float nms_threshold, const std::unordered_map<int, std::vector<int>>& stride_anchor_map) \
{
    // assert((outputTensors.size() % 2) == 0);
    std::shared_ptr<FastDecodeOutputYoloWorkLoad> context(new FastDecodeOutputYoloWorkLoad);
    if (outputTensors[0].second.GetDataType() == armnn::DataType::QSymmS8)
    {
        context->init<int8_t>(outputTensors, input_h, input_w, cls_num, prob_threshold, nms_threshold, stride_anchor_map);
        return [context](const armnn::OutputTensors& outputTensors) { return context->work<int8_t>(outputTensors); };
    }
    else if (outputTensors[0].second.GetDataType() == armnn::DataType::QAsymmU8)
    {
        context->init<uint8_t>(outputTensors, input_h, input_w, cls_num, prob_threshold, nms_threshold, stride_anchor_map);
        return [context](const armnn::OutputTensors& outputTensors) { return context->work<uint8_t>(outputTensors); };
    } else {
        throw std::runtime_error("the data type of output tensor is not supported");
        return {};
    }
}

struct FastDecodeOutputYoloKpsWorkLoad : public FastDecodeOutputYoloWorkLoad {
    template <typename T>
    std::vector<DetectObject> work(const armnn::OutputTensors &outputTensors) = delete;
    template <typename T>
    std::pair<std::vector<DetectObject>, std::vector<std::vector<std::vector<float>>>>
    workKps(const armnn::OutputTensors &outputTensors) {
        proposals_.clear();
        // same as generate_proposals
        for (int i = 0; i < head_num_; i++) {
            auto &out_tensor = outputTensors[i].second;
            int stride = input_w_ / out_tensor.GetShape()[data_layout_.GetWidthIndex()];
            if (stride_anchor_map_.find(stride) == stride_anchor_map_.end()) {
                LOG_ERROR("fail to find the stride: {} in stride anchor map", stride);
                throw std::runtime_error("fail to find the stride in stride anchor map\n");
            }
            std::vector<Object> objects_tmp;
            T *feat_data = (T *)out_tensor.GetMemoryArea();
            int feat_w = input_w_ / stride;
            int feat_h = input_h_ / stride;
            int anchor_num = stride_anchor_map_[stride].size() / 2;
            for (int h = 0; h <= feat_h - 1; h++) {
                for (int w = 0; w <= feat_w - 1; w++) {
                    for (int anchor = 0; anchor <= anchor_num - 1; anchor++) {
                        int class_index = 0;
                        T class_score = std::numeric_limits<T>::lowest();
                        int channel_size = feat_h * feat_w;
                        for (int s = 0; s <= cls_num_ - 1; s++) {
                            int score_index = anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + (s + 5 + 10) * channel_size;
                            T score = feat_data[score_index];
                            if (score > class_score) {
                                class_index = s;
                                class_score = score;
                            }
                        }
                        T box_score = feat_data[anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + 4 * channel_size];
                        float final_score = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&box_score)) * sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&class_score));
                        if (final_score >= prob_threshold_) {
                            int dx_index = anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + 0 * channel_size;
                            int dy_index = anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + 1 * channel_size;
                            int dw_index = anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + 2 * channel_size;
                            int dh_index = anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + 3 * channel_size;

                            T dx_tmp = feat_data[dx_index];
                            T dy_tmp = feat_data[dy_index];
                            T dw_tmp = feat_data[dw_index];
                            T dh_tmp = feat_data[dh_index];

                            float dx = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dx_tmp));
                            float dy = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dy_tmp));

                            // float dw = feat[dw_index];
                            // float dh = feat[dh_index];
                            float dw = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dw_tmp));
                            float dh = sigmoid_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&dh_tmp));

                            float anchor_w = stride_anchor_map_[stride][anchor * 2 + 0];
                            float anchor_h = stride_anchor_map_[stride][anchor * 2 + 1];

                            // float pred_x = (w + dx) * stride;
                            // float pred_y = (h + dy) * stride;
                            float pred_x = (dx * 2.0f - 0.5f + w) * stride;
                            float pred_y = (dy * 2.0f - 0.5f + h) * stride;
                            // float pred_w = exp(dw) * anchor_w;
                            // float pred_h = exp(dh) * anchor_h;
                            float pred_w = dw * dw * 4.0f * anchor_w;
                            float pred_h = dh * dh * 4.0f * anchor_h;

                            // float x0 = (pred_x - pred_w * 0.5f);
                            // float y0 = (pred_y - pred_h * 0.5f);
                            // float x1 = (pred_x + pred_w * 0.5f);
                            // float y1 = (pred_y + pred_h * 0.5f);

                            Object obj;
                            obj.rect.x = pred_x;
                            obj.rect.y = pred_y;
                            obj.rect.width = pred_w;
                            obj.rect.height = pred_h;
                            obj.label = class_index;
                            obj.prob = final_score;

                            // landmarks
                            for (unsigned int j = 0; j < 10; j += 2) {
                                cv::Point2f kps;
                                float kps_l = float_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&(
                                    feat_data[anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + (cls_num_ + 4 + j) * channel_size])));
                                float kps_t = float_tab_.operator[](i).operator[](*static_cast<uint8_t *>((void *)&(
                                    feat_data[anchor * (cls_num_ + 5 + 10) * channel_size + feat_w * h + w + (cls_num_ + 4 + j + 1) * channel_size])));

                                float kps_x = (kps_l)*anchor_w + w * stride; // cx + l x
                                float kps_y = (kps_t)*anchor_h + h * stride; // cy + t y
                                obj.kpt.push_back({kps_x, kps_y});
                            }
                            proposals_.push_back(obj);
                        }
                    }
                }
            }
        }
        // std::sort(proposals.begin(), proposals.end(), [](Object& a, Object& b) { return a.prob > b.prob; });
        qsort_descent_inplace(proposals_);
        std::vector<int> picked;
        nms_sorted_bboxes(proposals_, picked, nms_threshold_);

        int count = picked.size();
        // fprintf(stderr, "detection num: %d\n", count);
        std::vector<DetectObject> objects;
        std::vector<std::vector<std::vector<float>>> kps;
        objects.resize(count);
        kps.resize(count);
        for (int i = 0; i < count; i++) {
            objects[i].prob = proposals_[picked[i]].prob;
            objects[i].label = proposals_[picked[i]].label;
            objects[i].x = proposals_[picked[i]].rect.x;
            objects[i].y = proposals_[picked[i]].rect.y;
            objects[i].w = proposals_[picked[i]].rect.width;
            objects[i].h = proposals_[picked[i]].rect.height;
            kps[i] = std::move(proposals_[picked[i]].kpt);
        }
        return {objects, kps};
    }
};

std::function<std::pair<std::vector<DetectObject>, std::vector<std::vector<std::vector<float>>>>(const armnn::OutputTensors &outputTensors)>
getFastDecodeOutputYoloKpsWorkLoad(const armnn::OutputTensors &outputTensors, int input_h, int input_w, int cls_num, float prob_threshold,
                                   float nms_threshold, const std::unordered_map<int, std::vector<int>> &stride_anchor_map) {
    assert((outputTensors.size() % 2) == 0);
    std::shared_ptr<FastDecodeOutputYoloKpsWorkLoad> context(new FastDecodeOutputYoloKpsWorkLoad);
    if (outputTensors[0].second.GetDataType() == armnn::DataType::QSymmS8) {
        context->init<int8_t>(outputTensors, input_h, input_w, cls_num, prob_threshold, nms_threshold, stride_anchor_map);
        return [context](const armnn::OutputTensors &outputTensors) { return context->workKps<int8_t>(outputTensors); };
    } else if (outputTensors[0].second.GetDataType() == armnn::DataType::QAsymmU8) {
        context->init<uint8_t>(outputTensors, input_h, input_w, cls_num, prob_threshold, nms_threshold, stride_anchor_map);
        return [context](const armnn::OutputTensors &outputTensors) { return context->workKps<uint8_t>(outputTensors); };
    } else {
        throw std::runtime_error("the data type of output tensor is not supported");
        return {};
    }
}

struct FastDecodeOutputYolov8WorkLoad
{
    static constexpr int Focal_IC = 16;
    static constexpr int ltrb = 4;

    std::vector<Object> proposals_;

    std::vector<int> index_box_;
    std::vector<int> index_cls_;
    std::vector<std::vector<float>> exp_tab_;
    std::vector<std::vector<float>> sigmoid_tab_;
    int input_h_;
    int input_w_;
    int cls_num_;
    float prob_threshold_;
    float nms_threshold_;
    int head_num_;
    armnnUtils::DataLayoutIndexed data_layout_;

    FastDecodeOutputYolov8WorkLoad(): data_layout_(armnnUtils::DataLayoutIndexed(armnn::DataLayout::NCHW)) {}
    ~FastDecodeOutputYolov8WorkLoad() {}

    template <typename T>
    void init(const armnn::OutputTensors& outputTensors, int input_h, int input_w, int cls_num, float prob_threshold, float nms_threshold)
    {
        input_h_ = input_h;
        input_w_ = input_w;
        cls_num_ = cls_num;
        prob_threshold_ = prob_threshold;
        nms_threshold_ = nms_threshold;

        assert((outputTensors.size() % 2) == 0);
        head_num_ = outputTensors.size() / 2;
        index_box_.resize(head_num_, 0);
        index_cls_.resize(head_num_, 0);
        exp_tab_.resize(head_num_, std::vector<float>(256));
        sigmoid_tab_.resize(head_num_, std::vector<float>(256));

        std::map<int, int> stride_idx_map;
        for (int i = 0; i < (int)(outputTensors.size()); i++)
        {
            int stride = input_w_ / outputTensors.at(i).second.GetShape()[data_layout_.GetWidthIndex()];
            if (stride_idx_map.find(stride) == stride_idx_map.end()) {
                stride_idx_map[stride] = stride_idx_map.size();
            }

            if (outputTensors.at(i).second.GetShape()[data_layout_.GetChannelsIndex()] == Focal_IC*ltrb) {
                index_box_.at(stride_idx_map.at(stride)) = i;
            } else {
                index_cls_.at(stride_idx_map.at(stride)) = i;
            }
        }
        buildTable<T>(outputTensors);
    }

    template <typename T>
    std::vector<DetectObject> work(const armnn::OutputTensors& outputTensors)
    {
        proposals_.clear();
        // same as generate_proposals_v8
        for (int i = 0; i < head_num_; i++)
        {
            auto& box_tensor = outputTensors[index_box_[i]].second;
            auto& cls_tensor = outputTensors[index_cls_[i]].second;
            int stride = input_w_ / box_tensor.GetShape()[data_layout_.GetWidthIndex()];
            std::vector<Object> objects_tmp;
            T* cls_data = (T*)cls_tensor.GetMemoryArea();
            T* box_data = (T*)box_tensor.GetMemoryArea();
            int feat_w = input_w_ / stride;
            int feat_h = input_h_ / stride;
            for (int h = 0; h <= feat_h - 1; h++)
            {
                for (int w = 0; w <= feat_w - 1; w++)
                {
                    int class_index = 0;
                    T class_score = std::numeric_limits<T>::lowest();
                    int channel_size = feat_h * feat_w;
                    for (int s = 0; s <= cls_num_ - 1; s++)
                    {
                        int score_index = s * channel_size + feat_w * h + w;
                        T score = cls_data[score_index];
                        if (score > class_score)
                        {
                            class_index = s;
                            class_score = score;
                        }
                    }
                    float final_score = sigmoid_tab_[i][*static_cast<uint8_t*>((void*)&class_score)];
                    if (final_score >= prob_threshold_)
                    {
                        float dis_ltrb[ltrb];
                        for (int direction = 0; direction < ltrb; direction++)
                        {
                            dis_ltrb[direction] = 0;
                            T channels_value[Focal_IC];
                            float channels_value_softmax[Focal_IC];
                            float exp_sum = 0;
                            for (int c = 0; c < Focal_IC; c++)
                            {
                                channels_value[c] =
                                    box_data[direction * Focal_IC * channel_size + c * channel_size + feat_w * h + w];
                                channels_value_softmax[c] =
                                    exp_tab_[i][*static_cast<uint8_t*>((void*)&channels_value[c])];
                                exp_sum += channels_value_softmax[c];
                            }

                            // softmax(channels_value);
                            for (int c = 0; c < Focal_IC; c++)
                            {
                                channels_value_softmax[c] /= exp_sum;
                            }

                            for (int c = 0; c < Focal_IC; c++)
                            {
                                dis_ltrb[direction] += channels_value_softmax[c] * c;
                            }
                        }

                        float x0 = ((0.5f + w) - dis_ltrb[0]) * stride;
                        float y0 = ((0.5f + h) - dis_ltrb[1]) * stride;
                        float x1 = ((0.5f + w) + dis_ltrb[2]) * stride;
                        float y1 = ((0.5f + h) + dis_ltrb[3]) * stride;

                        Object obj;
                        obj.rect.x = x0;
                        obj.rect.y = y0;
                        obj.rect.width = x1 - x0;
                        obj.rect.height = y1 - y0;
                        obj.label = class_index;
                        obj.prob = final_score;
                        proposals_.push_back(obj);
                    }
                }
            }
        }

        // std::sort(proposals.begin(), proposals.end(), [](Object& a, Object& b) { return a.prob > b.prob; });
        qsort_descent_inplace(proposals_);
        std::vector<int> picked;
        nms_sorted_bboxes(proposals_, picked, nms_threshold_);

        int count = picked.size();
        // fprintf(stderr, "detection num: %d\n", count);
        std::vector<DetectObject> objects;
        objects.resize(count);
        for (int i = 0; i < count; i++)
        {
            objects[i].prob = proposals_[picked[i]].prob;
            objects[i].label = proposals_[picked[i]].label;
            objects[i].x = proposals_[picked[i]].rect.x + proposals_[picked[i]].rect.width / 2;
            objects[i].y = proposals_[picked[i]].rect.y + proposals_[picked[i]].rect.height / 2;
            objects[i].w = proposals_[picked[i]].rect.width;
            objects[i].h = proposals_[picked[i]].rect.height;
            LOG_DEBUG("detect item {}:  x_center: {}, y_center: {}, w: {}, h: {}, prob: {}, cls: {}\n", i, objects[i].x, objects[i].y, objects[i].w, objects[i].h, objects[i].prob, objects[i].label);
        }
        return objects;
    }
    static inline float sigmoid(float x)
    {
        return static_cast<float>(1.f / (1.f + exp(-x)));
    }
    template <typename T>
    void buildTable(const armnn::OutputTensors& outputTensors)
    {
        for (int x = 0; x < head_num_; x++)
        {
            armnn::Tensor Tensor0 = outputTensors[index_box_[x]].second;
            armnn::Tensor Tensor1 = outputTensors[index_cls_[x]].second;
            {
                float scale = Tensor1.GetInfo().GetQuantizationScale();
                int zero_point = Tensor1.GetInfo().GetQuantizationOffset();
                for (int i = std::numeric_limits<T>::lowest(); i <= std::numeric_limits<T>::max(); i++)
                {
                    sigmoid_tab_[x][*static_cast<uint8_t*>((void*)&i)] = sigmoid((float(i) - zero_point) * scale);
                }
            }
            {
                float scale = Tensor0.GetInfo().GetQuantizationScale();
                int zero_point = Tensor0.GetInfo().GetQuantizationOffset();
                for (int i = std::numeric_limits<T>::lowest(); i <= std::numeric_limits<T>::max(); i++)
                {
                    exp_tab_[x][*static_cast<uint8_t*>((void*)&i)] = exp((float(i) - zero_point) * scale);
                }
            }
        }
    }
};

std::function<std::vector<DetectObject>(const armnn::OutputTensors& outputTensors)>
    getFastDecodeOutputYolov8WorkLoad(const armnn::OutputTensors& outputTensors, int input_h, int input_w, int cls_num, float prob_threshold, float nms_threshold)
{
    assert((outputTensors.size() % 2) == 0);
    std::shared_ptr<FastDecodeOutputYolov8WorkLoad> context(new FastDecodeOutputYolov8WorkLoad);
    if (outputTensors[0].second.GetDataType() == armnn::DataType::QSymmS8)
    {
        context->init<int8_t>(outputTensors, input_h, input_w, cls_num, prob_threshold, nms_threshold);
        return [context](const armnn::OutputTensors& outputTensors) { return context->work<int8_t>(outputTensors); };
    }
    else if (outputTensors[0].second.GetDataType() == armnn::DataType::QAsymmU8)
    {
        context->init<uint8_t>(outputTensors, input_h, input_w, cls_num, prob_threshold, nms_threshold);
        return [context](const armnn::OutputTensors& outputTensors) { return context->work<uint8_t>(outputTensors); };
    } else {
        throw std::runtime_error("the data type of output tensor is not supported");
        return {};
    }
}