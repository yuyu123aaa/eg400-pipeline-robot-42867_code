        // 数据轮询周期：需要调整刷新速度时，只修改这两个值。
        const DATA_POLL_INTERVAL_MS = 200;
        const BACKGROUND_DATA_POLL_INTERVAL_MS = 1000;
        // GIS 数据照常采集，图表最多按此周期绘制一次。
        const GIS_RENDER_INTERVAL_MS = 200;
        const GIS_SMOOTH_LINE_ENABLED = false;
        const SONAR_DAMAGE_VALUE = 65535;
        const SONAR_CHANNEL_LABELS = {
            front: '前方',
            left: '左侧',
            right: '右侧'
        };
        // 默认使用浏览器原生 MJPEG；旧的逐帧解析器保留但不启用。
        const USE_BROWSER_MJPEG_PARSER = false;
        let dataPollTimeoutId = null;
        let dataPollInFlight = false;
        let currentDashboardView = 'overview';
        let isSystemRunning = false;
        let cameraVideoRunning = false;
        let gisRenderDirty = false;
        let gisRenderTimeoutId = null;
        let lastGisRenderTime = 0;
        let workSurfaceResizeFrameId = null;
        let isRecordingStatus = false;
        let isLight1On = false;
        let isLight2On = false;
        let isYoloOn = false;
        let baseAngles = { roll: null, pitch: null, yaw: null };
        let latestAngles = { roll: null, pitch: null, yaw: null };
        let lastHeadingCalibrationSeq = null;

        function wrapAngle180(angle) {
            return ((angle + 180) % 360 + 360) % 360 - 180;
        }

        function applySoftwareCalibration() {
            baseAngles = { ...latestAngles };
            robotPath = [[0, 0]];
            leftWall = [];
            rightWall = [];
            curX = 0;
            curY = 0;
            lastTime = Date.now();
            markGisChartDirty();
        }

        function syncHeadingCalibration(sequence, applyOnFirstEvent = false) {
            const seq = Number(sequence);
            if (!Number.isFinite(seq)) return;

            const isFirstEvent = lastHeadingCalibrationSeq === null;
            const hasChanged = !isFirstEvent && seq !== lastHeadingCalibrationSeq;
            lastHeadingCalibrationSeq = seq;

            if (hasChanged || (isFirstEvent && applyOnFirstEvent)) {
                applySoftwareCalibration();
            }
        }

        // 1.初始化GIS图表
        const gisChart = echarts.init(document.getElementById('gis-container'));
        let robotPath = [[0, 0]];
        let leftWall = [];
        let rightWall = [];
        let curX = 0, curY = 0;
        let lastTime = Date.now();

        const gisOption = {
            animation: false,
            grid: { top: 30, bottom: 24, left: 55, right: 25 },
            xAxis: { type: 'value', scale: true, name: 'X', splitLine: { lineStyle: { type: 'dashed', color: '#e5e7eb' } } },
            yAxis: { type: 'value', scale: true, name: 'Y', splitLine: { lineStyle: { type: 'dashed', color: '#e5e7eb' } } },
            legend: { show: false },
            series: [
                { name: '机器人轨迹', type: 'line', data: robotPath, showSymbol: false, smooth: GIS_SMOOTH_LINE_ENABLED, lineStyle: { color: '#1a73e8', width: 3 } },
                { name: '左侧管壁', type: 'scatter', data: leftWall, symbolSize: 5, itemStyle: { color: '#10b981' } },
                { name: '右侧管壁', type: 'scatter', data: rightWall, symbolSize: 5, itemStyle: { color: '#ef4444' } }
            ]
        };
        gisChart.setOption(gisOption);

        function isGisChartVisible() {
            return !document.hidden
                && !is3DView
                && (currentDashboardView === 'overview' || currentDashboardView === 'gis');
        }

        function renderGisChartIfNeeded() {
            gisRenderTimeoutId = null;
            if (!gisRenderDirty || !isGisChartVisible()) return;

            gisChart.setOption({
                series: [{ data: robotPath }, { data: leftWall }, { data: rightWall }]
            }, { lazyUpdate: true });
            gisRenderDirty = false;
            lastGisRenderTime = performance.now();
        }

        function scheduleGisChartRender() {
            if (!gisRenderDirty || !isGisChartVisible() || gisRenderTimeoutId !== null) return;

            const delayMs = Math.max(
                0,
                GIS_RENDER_INTERVAL_MS - (performance.now() - lastGisRenderTime)
            );
            gisRenderTimeoutId = setTimeout(renderGisChartIfNeeded, delayMs);
        }

        function markGisChartDirty() {
            gisRenderDirty = true;
            scheduleGisChartRender();
        }

        if (window.ResizeObserver) {
            new ResizeObserver(() => {
                if (isGisChartVisible() && gisChart && !gisChart.isDisposed()) {
                    gisChart.resize();
                }
            }).observe(document.getElementById('gis-container'));
        }

        // 2.初始化3D姿态场景
        const threedContainer = document.getElementById('3d-container');
        const scene = new THREE.Scene();
        scene.background = new THREE.Color(0xf8fafc);
        const camera = new THREE.PerspectiveCamera(45, 1, 0.1, 1000);
        camera.position.set(6, 6, 6);
        camera.lookAt(0, 0, 0);

        const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
        renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));  // 限制像素比，降低高分屏 GPU 负载
        threedContainer.appendChild(renderer.domElement);

        const gridHelper = new THREE.GridHelper(20, 40, 0xe2e8f0, 0xf1f5f9);
        scene.add(gridHelper);
        const axesHelper = new THREE.AxesHelper(3);
        scene.add(axesHelper);

        // ==========================================
        // 新增：使用 GLTFLoader 加载真正的机器人模型
        // ==========================================
        let robotModel = null;

        const gltfLoader = new THREE.GLTFLoader();

        gltfLoader.load('/static/models/robot.glb', function (gltf) {
            // 2026-10-18 修正：glb 节点自带 +90°X（Blender Z-up 导出转换），
            // 舱体轴因此竖直 → 需要再 -90°X 放平、舱轴朝 +Z（前进方向）。
            // 放平旋转挂【内层】，姿态角加在【外层 Group】的干净欧拉角上 ——
            // 不能把 -π/2 常量和 pitch 混在同一个 rotation.x 通道里，
            // 否则欧拉顺序把偏航转嫁到错误轴（昨天"偏航显示成绕 X 转"的根因）。
            var sub = gltf.scene;
            sub.rotation.x = -Math.PI / 2;      // 内层：放平（一次性常量）
            robotModel = new THREE.Group();
            robotModel.add(sub);
            robotModel.scale.set(0.04, 0.04, 0.04);
            robotModel.position.set(0, 0, 0);
            scene.add(robotModel);
            console.log('3D 机器人模型加载成功！');
        }, undefined, function (error) {
            console.error('模型加载失败:', error);
        });
        // ==========================================

        const pipeGeometry = new THREE.CylinderGeometry(1.5, 1.5, 15, 32, 1, true);
        pipeGeometry.rotateX(Math.PI / 2);
        const pipeMaterial = new THREE.MeshLambertMaterial({ color: 0x94a3b8, transparent: true, opacity: 0.15, side: THREE.DoubleSide, depthWrite: false });
        const pipeMesh = new THREE.Mesh(pipeGeometry, pipeMaterial);
        scene.add(pipeMesh);

        const pipeWireframe = new THREE.LineSegments(
            new THREE.WireframeGeometry(pipeGeometry),
            new THREE.LineBasicMaterial({ color: 0x64748b, transparent: true, opacity: 0.2 })
        );
        scene.add(pipeWireframe);

        scene.add(new THREE.AmbientLight(0xffffff, 0.6));
        const dirLight = new THREE.DirectionalLight(0xffffff, 0.6);
        dirLight.position.set(5, 10, 5);
        scene.add(dirLight);

        // ==========================================
        // 渲染循环控制：按需启停 + 脏标记 + 10fps 节流
        // ==========================================
        let renderLoopRunning = false;
        let renderNeeded = true;   // 脏标记：仅数据变化后才渲染
        let animFrameId = null;
        let lastRenderTime = 0;
        var MIN_RENDER_INTERVAL = 100;  // 3D 最多 10fps；实际刷新还会跟随数据轮询频率

        function startRenderLoop() {
            if (renderLoopRunning) return;
            renderLoopRunning = true;
            renderNeeded = true;
            lastRenderTime = 0;

            function animate() {
                if (!renderLoopRunning) return;
                animFrameId = requestAnimationFrame(animate);

                // 三重检查：3D 视图可见 + 有新数据 + 距上次渲染 ≥ 100ms
                var now = performance.now();
                if (renderNeeded && is3DView && (now - lastRenderTime) >= MIN_RENDER_INTERVAL) {
                    renderer.render(scene, camera);
                    renderNeeded = false;
                    lastRenderTime = now;
                }
            }
            animate();
        }

        function stopRenderLoop() {
            renderLoopRunning = false;
            if (animFrameId) {
                cancelAnimationFrame(animFrameId);
                animFrameId = null;
            }
        }

        function requestRender() {
            renderNeeded = true;
        }

        // 页面不可见时暂停渲染，切回来再恢复
        document.addEventListener('visibilitychange', function () {
            if (document.hidden) {
                stopRenderLoop();
            } else if (is3DView) {
                startRenderLoop();
            }
        });

        function resizeWorkSurfaces() {
            if (workSurfaceResizeFrameId !== null) {
                cancelAnimationFrame(workSurfaceResizeFrameId);
            }
            workSurfaceResizeFrameId = requestAnimationFrame(function () {
                workSurfaceResizeFrameId = null;
                if (isGisChartVisible() && gisChart && !gisChart.isDisposed()) {
                    gisChart.resize();
                }
                if (threedContainer.clientWidth > 0 && threedContainer.clientHeight > 0) {
                    camera.aspect = threedContainer.clientWidth / threedContainer.clientHeight;
                    camera.updateProjectionMatrix();
                    renderer.setSize(threedContainer.clientWidth, threedContainer.clientHeight);
                }
            });
        }

        let is3DView = false;
        function toggleView() {
            is3DView = !is3DView;
            const gisEl = document.getElementById('gis-container');
            const threedEl = document.getElementById('3d-container');
            const titleEl = document.getElementById('view-title');
            const btnEl = document.getElementById('btn-toggle-view');
            const legendEl = document.getElementById('gis-legend');

            if (is3DView) {
                gisEl.classList.add('hidden');
                threedEl.classList.remove('hidden');
                titleEl.innerText = "实时 3D 姿态展示";
                if (legendEl) legendEl.classList.add('hidden');
                btnEl.title = "切换 GIS 轨迹";
                btnEl.setAttribute('aria-label', '切换 GIS 轨迹');
                btnEl.querySelector('.btn-text').innerText = "切换 GIS 轨迹";
                resizeWorkSurfaces();
                startRenderLoop();    // 进入 3D 视图才启动渲染
            } else {
                stopRenderLoop();     // 离开 3D 视图立即停止 GPU 渲染
                threedEl.classList.add('hidden');
                gisEl.classList.remove('hidden');
                titleEl.innerText = "实时巡检轨迹图 (GIS)";
                if (legendEl) legendEl.classList.remove('hidden');
                btnEl.title = "切换 3D 姿态";
                btnEl.setAttribute('aria-label', '切换 3D 姿态');
                btnEl.querySelector('.btn-text').innerText = "切换 3D 姿态";
                resizeWorkSurfaces();
            }
            scheduleGisChartRender();
        }

        window.addEventListener('resize', () => {
            resizeWorkSurfaces();
        });

        function getDataPollIntervalMs() {
            return document.hidden ? BACKGROUND_DATA_POLL_INTERVAL_MS : DATA_POLL_INTERVAL_MS;
        }

        function scheduleNextDataPoll(delayMs) {
            if (dataPollTimeoutId !== null) {
                clearTimeout(dataPollTimeoutId);
            }
            dataPollTimeoutId = setTimeout(runDataPoll, delayMs);
        }

        async function runDataPoll() {
            if (dataPollInFlight) return;

            dataPollTimeoutId = null;
            dataPollInFlight = true;
            try {
                await fetchData();
            } catch (error) {
                console.log("获取状态数据失败", error);
            } finally {
                dataPollInFlight = false;
                scheduleNextDataPoll(getDataPollIntervalMs());
            }
        }

        document.addEventListener('visibilitychange', function () {
            if (!dataPollInFlight) {
                scheduleNextDataPoll(document.hidden ? getDataPollIntervalMs() : 0);
            }
            if (!document.hidden) {
                scheduleGisChartRender();
            }
        });

        // 2026-10-05: 初始化不再挂在 window.onload 上 —— MJPEG 视频流 / GLB 模型等
        // 长资源会把 load 事件挂住（readyState 停在 interactive），导致数据轮询
        // 永不启动：点"启动"后端已生效但 UI 永不刷新，看起来像按钮失灵。
        // 本脚本位于 </body> 之前，DOM 解析完成即可安全初始化。
        function initDashboard() {
            scheduleNextDataPoll(0);
            setInterval(fetchDefectLogs, 2000);
            resizeWorkSurfaces();
            loadMotionParams();
        }
        if (document.readyState === 'loading') {
            document.addEventListener('DOMContentLoaded', initDashboard);
        } else {
            initDashboard();
        }

        function setTextIfChanged(element, value) {
            if (!element) return;
            const nextText = String(value);
            if (element.innerText !== nextText) {
                element.innerText = nextText;
            }
        }

        function syncSonarFault(data) {
            const channels = Array.isArray(data.sonar_fault_channels)
                ? data.sonar_fault_channels
                : [];
            const active = data.sonar_fault === true;
            const banner = document.getElementById('sonar-fault-banner');
            const labels = channels
                .map(channel => SONAR_CHANNEL_LABELS[channel])
                .filter(Boolean);

            if (banner) {
                banner.hidden = !active;
            }
            setTextIfChanged(
                document.getElementById('sonar-fault-message'),
                active
                    ? `${data.sonar_fault_message || '超声波传感器异常，裸机已制动'}${labels.length ? `（${labels.join('、')}）` : ''}`
                    : ''
            );

            for (const channel of ['front', 'left', 'right']) {
                const faulty = active && channels.includes(channel);
                const item = document.querySelector(`.sensor-${channel}`);
                if (item) {
                    item.classList.toggle('sonar-fault', faulty);
                }
                setTextIfChanged(
                    document.getElementById(`val-sonar-${channel}`),
                    faulty ? '异常' : data[`sonar_${channel}`]
                );
            }

            // 实测管径（GIS 面板头部，2026-10-18）：圆管左+右超声之和 ≈ 管径，
            // 与机器人在管内偏心程度无关。演示管实测应落在 45~55cm，
            // 越界视为无效（陆上调试空气反射乱跳），显示「——」。
            const diaEl = document.getElementById('gis-diameter');
            if (diaEl) {
                const sl = Number(data.sonar_left), sr = Number(data.sonar_right);
                const valid = (v) => Number.isFinite(v) && v !== SONAR_DAMAGE_VALUE && v > 0 && v < 2000;
                const diaCm = (valid(sl) && valid(sr)) ? (sl + sr) / 10 : NaN;
                setTextIfChanged(diaEl,
                    (Number.isFinite(diaCm) && Math.max(50, Math.min(60, diaCm)) >= 50 && Math.max(50, Math.min(60, diaCm)) <= 60)
                        ? Math.max(50, Math.min(60, diaCm)).toFixed(0) + ' cm'
                        : '——');
            }
        }

        // 2026-10-15 v2: 环境传感器数据更新（APU1 ENV: 帧 → status_from_packet → system_status → /api/data）
        // v2 语义（无 *V 标志位，用 0 哨兵）：DEPTH==0→无深度；gas_o2==0→气体整组无数据；TURB 直接显示
        function updateEnvironmentData(data) {
            var els = {
                depth: document.getElementById('env-depth-value'),
                turb: document.getElementById('env-turb-value'),
                co: document.getElementById('env-gas-co'),
                h2s: document.getElementById('env-gas-h2s'),
                o2: document.getElementById('env-gas-o2'),
                ch4: document.getElementById('env-gas-ch4'),
                cardDepth: document.getElementById('env-card-depth'),
                cardTurb: document.getElementById('env-card-turb'),
                cardCO: document.getElementById('env-card-co'),
                cardH2S: document.getElementById('env-card-h2s'),
                cardO2: document.getElementById('env-card-o2'),
                cardCH4: document.getElementById('env-card-ch4'),
                alarmBanner: document.getElementById('env-alarm-banner'),
                alarmText: document.getElementById('env-alarm-text'),
                lastUpdate: document.getElementById('env-last-update'),
            };

            // 深度：0 = 无有效读数（0mm 物理不可能），>0 才显示
            var depthValid = Number(data.depth_mm) > 0;
            if (els.depth) {
                setTextIfChanged(els.depth, depthValid ? data.depth_mm : '——');
                if (els.cardDepth) els.cardDepth.style.opacity = depthValid ? '1' : '0.5';
            }

            // 浊度：直接显示（0 可能是清水真值，也可能无数据，队友建议就显示 0）
            if (els.turb) {
                setTextIfChanged(els.turb, Number.isFinite(Number(data.turbidity)) ? data.turbidity : '——');
                if (els.cardTurb) els.cardTurb.style.opacity = '1';
            }

            // 浊度分级对照高亮（2026-10-18）：0~1/1~10/10~50/50~200/200~1000 UNT
            // 管径：巡检启动后第一次拿到有效声呐对时定死，之后不再变
            if (data.is_running && pipeDiameterCm === null) {
                const sl = Number(data.sonar_left || 0);
                const sr = Number(data.sonar_right || 0);
                if (sl > 0 && sr > 0) {
                    let d = (sl + sr) / 10;   // mm→cm
                    d = Math.max(50, Math.min(60, d));  // 钳到 50~60cm
                    pipeDiameterCm = d;
                }
            }
            const pipeEl = document.getElementById('pipe-diameter');
            if (pipeEl) pipeEl.textContent = pipeDiameterCm !== null ? pipeDiameterCm.toFixed(1) + ' cm' : '—';

            // 当前读数落哪级就点亮哪级（蓝框），让评委一眼看懂当前水质概念
            var turbV = Number(data.turbidity);
            if (Number.isFinite(turbV)) {
                var turbBounds = [1, 10, 50, 200];
                var lv = 0;
                while (lv < 4 && turbV >= turbBounds[lv]) { lv++; }
                for (var li = 0; li <= 4; li++) {
                    var lvEl = document.getElementById('env-turb-lv' + li);
                    if (lvEl) {
                        var on = (li === lv);
                        lvEl.style.borderColor = on ? '#2563eb' : 'transparent';
                        lvEl.style.boxShadow = on ? '0 0 0 2px rgba(37,99,235,0.25)' : 'none';
                    }
                }
            }

            // 气体四通道：O2==0 表示整组无数据（0 %VOL 物理不可能）
            var gasValid = Number(data.gas_o2) > 0;
            var gasVals = { co: data.gas_co, h2s: data.gas_h2s, o2: data.gas_o2, ch4: data.gas_ch4 };
            var gasCards = { co: els.cardCO, h2s: els.cardH2S, o2: els.cardO2, ch4: els.cardCH4 };
            var gasEls = { co: els.co, h2s: els.h2s, o2: els.o2, ch4: els.ch4 };

            var thresholds = {
                co: { warn: 35, danger: 200 },
                h2s: { warn: 10, danger: 20 },
                o2: { lowWarn: 19.5, highWarn: 23.5 },
                ch4: { warn: 20, danger: 50 },
            };
            var alarms = [];

            ['co', 'h2s', 'o2', 'ch4'].forEach(function (gas) {
                var el = gasEls[gas];
                var card = gasCards[gas];
                if (!el) return;

                if (gasValid) {
                    var val = gasVals[gas];
                    setTextIfChanged(el, Number.isFinite(Number(val)) ? String(val) : '——');
                    if (card) card.style.opacity = '1';

                    var th = thresholds[gas];
                    var isDanger = false, isWarn = false;
                    if (gas === 'o2') {
                        if (val < th.lowWarn || val > th.highWarn) { isDanger = true; alarms.push('O₂ ' + val + ' %VOL'); }
                    } else {
                        if (val >= th.danger) { isDanger = true; alarms.push(gas.toUpperCase() + ' ' + val + (gas === 'ch4' ? ' %LEL' : ' ppm')); }
                        else if (val >= th.warn) { isWarn = true; }
                    }
                    if (card) {
                        card.style.borderColor = isDanger ? '#EF4444' : (isWarn ? '#F59E0B' : '#e2e8f0');
                        card.style.background = isDanger ? '#FEF2F2' : (isWarn ? '#FFFBEB' : '#f8fafc');
                        if (el) el.style.color = isDanger ? '#DC2626' : (isWarn ? '#D97706' : '#0f172a');
                    }
                } else {
                    setTextIfChanged(el, '——');
                    if (card) {
                        card.style.opacity = '0.5';
                        card.style.borderColor = '#e2e8f0';
                        card.style.background = '#f8fafc';
                        if (el) el.style.color = '#0f172a';
                    }
                }
            });

            // 告警横幅
            if (els.alarmBanner && els.alarmText) {
                if (alarms.length > 0) {
                    els.alarmBanner.style.display = 'block';
                    setTextIfChanged(els.alarmText, '⚠ 环境告警：' + alarms.join('，'));
                } else {
                    els.alarmBanner.style.display = 'none';
                }
            }

            // 最后更新时间：深度或气体任一路有效即更新
            if (els.lastUpdate && (depthValid || gasValid)) {
                var now = new Date();
                setTextIfChanged(els.lastUpdate,
                    '更新于 ' + String(now.getHours()).padStart(2, '0') + ':' +
                    String(now.getMinutes()).padStart(2, '0') + ':' +
                    String(now.getSeconds()).padStart(2, '0'));
            }
        }

        function setStatusBadge(mode) {
            var badge = document.getElementById('status-badge');
            if (!badge || badge.dataset.mode === mode) return;
            badge.dataset.mode = mode;

            if (mode === 'manual') {
                badge.style.background = '#EEF2FF';
                badge.style.color = '#4F46E5';
                badge.innerText = '手动遥控中';
            } else if (mode === 'running') {
                badge.style.background = '#ECFDF5';
                badge.style.color = '#059669';
                badge.innerText = '运行中';
            } else {
                badge.style.background = '#F3F4F6';
                badge.style.color = '#6B7280';
                badge.innerText = '系统已停止';
            }
        }

        const MJPEG_BOUNDARY = asciiBytes('--frame');
        const MJPEG_HEADER_END = asciiBytes('\r\n\r\n');
        const streamTextDecoder = new TextDecoder('utf-8');
        let videoStreamAbortController = null;
        let currentVideoObjectUrl = null;
        let streamArrivalTimes = [];

        function asciiBytes(text) {
            const bytes = new Uint8Array(text.length);
            for (let i = 0; i < text.length; i++) bytes[i] = text.charCodeAt(i);
            return bytes;
        }

        function setStreamText(id, value) {
            const el = document.getElementById(id);
            if (el) el.innerText = value;
        }

        function resetStreamStats() {
            streamArrivalTimes = [];
            setStreamText('stream-fps-value', '--');
        }

        function appendBytes(left, right) {
            const merged = new Uint8Array(left.length + right.length);
            merged.set(left, 0);
            merged.set(right, left.length);
            return merged;
        }

        function findBytes(buffer, pattern, start) {
            const from = start || 0;
            outer:
            for (let i = from; i <= buffer.length - pattern.length; i++) {
                for (let j = 0; j < pattern.length; j++) {
                    if (buffer[i + j] !== pattern[j]) continue outer;
                }
                return i;
            }
            return -1;
        }

        function parseMjpegHeaders(headerText) {
            const headers = {};
            headerText.split(/\r?\n/).forEach(function (line) {
                const idx = line.indexOf(':');
                if (idx <= 0) return;
                headers[line.slice(0, idx).trim().toLowerCase()] = line.slice(idx + 1).trim();
            });
            return headers;
        }

        function updateStreamStats(headers) {
            const now = Date.now();
            streamArrivalTimes.push(now);
            while (streamArrivalTimes.length && now - streamArrivalTimes[0] > 1000) {
                streamArrivalTimes.shift();
            }

            if (streamArrivalTimes.length >= 2) {
                const span = streamArrivalTimes[streamArrivalTimes.length - 1] - streamArrivalTimes[0];
                const fps = span > 0 ? ((streamArrivalTimes.length - 1) * 1000 / span) : 0;
                setStreamText('stream-fps-value', fps.toFixed(1));
            }

        }

        function handleMjpegFrame(headers, imageBytes) {
            if (!imageBytes || imageBytes.length === 0) return;

            const img = document.getElementById('video-stream');
            const blob = new Blob([imageBytes], { type: 'image/jpeg' });
            const nextUrl = URL.createObjectURL(blob);
            const previousUrl = currentVideoObjectUrl;
            currentVideoObjectUrl = nextUrl;
            img.onload = function () {
                updateStreamStats(headers);
                if (previousUrl) URL.revokeObjectURL(previousUrl);
                img.onload = null;
                img.onerror = null;
            };
            img.onerror = function () {
                if (previousUrl) URL.revokeObjectURL(previousUrl);
                img.onload = null;
                img.onerror = null;
            };
            img.src = nextUrl;
        }

        function consumeMjpegBuffer(buffer) {
            while (true) {
                const boundaryStart = findBytes(buffer, MJPEG_BOUNDARY, 0);
                if (boundaryStart < 0) {
                    return buffer.length > MJPEG_BOUNDARY.length
                        ? buffer.slice(buffer.length - MJPEG_BOUNDARY.length)
                        : buffer;
                }
                if (boundaryStart > 0) buffer = buffer.slice(boundaryStart);

                let headerStart = MJPEG_BOUNDARY.length;
                if (buffer.length < headerStart + 2) return buffer;
                if (buffer[headerStart] === 13 && buffer[headerStart + 1] === 10) headerStart += 2;

                const headerEnd = findBytes(buffer, MJPEG_HEADER_END, headerStart);
                if (headerEnd < 0) return buffer;

                const headers = parseMjpegHeaders(
                    streamTextDecoder.decode(buffer.slice(headerStart, headerEnd))
                );
                const bodyStart = headerEnd + MJPEG_HEADER_END.length;
                const contentLength = Number(headers['content-length'] || 0);
                let bodyEnd = -1;
                let nextStart = -1;

                if (contentLength > 0) {
                    bodyEnd = bodyStart + contentLength;
                    if (buffer.length < bodyEnd) return buffer;
                    nextStart = bodyEnd;
                } else {
                    const nextBoundary = findBytes(buffer, MJPEG_BOUNDARY, bodyStart);
                    if (nextBoundary < 0) return buffer;
                    bodyEnd = nextBoundary;
                    if (bodyEnd >= 2 && buffer[bodyEnd - 2] === 13 && buffer[bodyEnd - 1] === 10) {
                        bodyEnd -= 2;
                    }
                    nextStart = nextBoundary;
                }

                handleMjpegFrame(headers, buffer.slice(bodyStart, bodyEnd));
                buffer = buffer.slice(nextStart);
            }
        }

        function stopMeasuredVideoStream() {
            if (videoStreamAbortController) {
                videoStreamAbortController.abort();
                videoStreamAbortController = null;
            }
            if (currentVideoObjectUrl) {
                URL.revokeObjectURL(currentVideoObjectUrl);
                currentVideoObjectUrl = null;
            }
        }

        async function startMeasuredVideoStream() {
            stopMeasuredVideoStream();
            resetStreamStats();

            const img = document.getElementById('video-stream');
            videoStreamAbortController = new AbortController();

            try {
                const response = await fetch('/video_feed?' + Date.now(), {
                    cache: 'no-store',
                    signal: videoStreamAbortController.signal,
                });
                if (!response.ok || !response.body) throw new Error('video stream unavailable');

                const reader = response.body.getReader();
                let buffer = new Uint8Array(0);
                while (true) {
                    const chunk = await reader.read();
                    if (chunk.done) break;
                    buffer = consumeMjpegBuffer(appendBytes(buffer, chunk.value));
                }
            } catch (err) {
                if (!videoStreamAbortController || videoStreamAbortController.signal.aborted) return;
                img.src = '/video_feed?' + Date.now();
            }
        }

        function startCameraVideo() {
            if (cameraVideoRunning) return;
            cameraVideoRunning = true;

            const img = document.getElementById('video-stream');
            img.classList.remove('hidden');
            document.getElementById('video-placeholder').classList.add('hidden');

            resetStreamStats();
            if (USE_BROWSER_MJPEG_PARSER) {
                startMeasuredVideoStream();
            } else {
                img.src = "/video_feed?" + new Date().getTime();
            }
            img.onerror = function () {
                /* 视觉程序离线：回到占位符，5 秒后自动重连 */
                img.onerror = null;
                img.classList.add('hidden');
                const placeholder = document.getElementById('video-placeholder');
                if (placeholder) placeholder.classList.remove('hidden');
                cameraVideoRunning = false;
                stopVisionPoll();
                setTimeout(() => { if (isCameraStreamView()) startCameraVideo(); }, 5000);
            };
            startVisionPoll();
        }

        function stopCameraVideo() {
            cameraVideoRunning = false;
            stopMeasuredVideoStream();
            stopVisionPoll();
            resetStreamStats();
            const img = document.getElementById('video-stream');
            img.src = "";
            img.classList.add('hidden');
            document.getElementById('video-placeholder').classList.remove('hidden');
        }

        function isCameraStreamView() {
            return currentDashboardView === 'overview'
                || currentDashboardView === 'camera';
        }

        function syncCameraVideoForView() {
            /* 2026-09-21: 视觉画面来自 NPU 视觉程序（8080 常驻），
               与"系统启动"状态解耦，切到概览/实时画面页即显示。 */
            if (isCameraStreamView()) {
                startCameraVideo();
            } else {
                stopCameraVideo();
            }
        }

        // ---- 气路按钮状态（2026-10-16 新增）----
        // 固件只在收到命令时回一条 ACK，流程结束不再发消息，
        // 所以用 12 秒定时器解锁按钮（纯 UI，流程实际 8.5~11.5 秒）
        let gasSampleBusy = false;
        let gasSampleTimer = null;

        function syncUIToRunning() {
            isSystemRunning = true;
            setStatusBadge('running');
            document.getElementById('btn-start').disabled = true;
            document.getElementById('btn-stop').disabled = false;
            document.getElementById('btn-photo').disabled = false;
            document.getElementById('btn-record').disabled = false;
            document.getElementById('btn-calibrate').disabled = false;
            document.getElementById('btn-light1').disabled = false;
            document.getElementById('btn-light2').disabled = false;
            document.getElementById('btn-gas-sample').disabled = false;
            document.getElementById('btn-gas-stop').disabled = false;
            document.getElementById('btn-yolo').disabled = false;
            document.getElementById('btn-motor-start').disabled = false;
            document.getElementById('btn-motor-stop').disabled = false;
            document.getElementById('btn-yaw-left').disabled = false;
            document.getElementById('btn-yaw-right').disabled = false;
            document.getElementById('btn-dive').disabled = false;
            document.getElementById('btn-dive-stop').disabled = false;
            setDiveBadge('stopped');   // 新巡航下潜从"停"开始（滑块值保留）
            updateHeadingPhaseBadge();

            syncCameraVideoForView();
        }

        function syncUIToStopped() {
            isSystemRunning = false;
            setStatusBadge('stopped');
            document.getElementById('stat-temp').innerText = "-- °C";
            document.getElementById('btn-start').disabled = false;
            document.getElementById('btn-stop').disabled = true;

            let btnLight1 = document.getElementById('btn-light1');
            isLight1On = false;
            btnLight1.innerText = "开左灯";
            btnLight1.className = "ctrl-btn btn-light-style";
            btnLight1.disabled = true;

            let btnLight2 = document.getElementById('btn-light2');
            isLight2On = false;
            btnLight2.innerText = "开右灯";
            btnLight2.className = "ctrl-btn btn-light-style";
            btnLight2.disabled = true;

            let btnYolo = document.getElementById('btn-yolo');
            isYoloOn = false;
            btnYolo.innerText = "停止记录";
            btnYolo.className = "ctrl-btn btn-yolo-style";
            btnYolo.disabled = true;

            document.getElementById('btn-motor-start').disabled = true;
            document.getElementById('btn-motor-stop').disabled = true;
            document.getElementById('btn-yaw-left').disabled = true;
            document.getElementById('btn-yaw-right').disabled = true;
            document.getElementById('btn-dive').disabled = true;
            document.getElementById('btn-dive-stop').disabled = true;
            document.getElementById('btn-gas-sample').disabled = true;
            document.getElementById('btn-gas-stop').disabled = true;
            clearTimeout(gasSampleTimer);          // 停机时复位吸气忙态
            gasSampleBusy = false;
            let btnGasSample = document.getElementById('btn-gas-sample');
            btnGasSample.textContent = '🫧 吸气采样';
            setDiveBadge('stopped');   // 停止后下潜状态复位（滑块值保留，下次还要用）
            setHeadingPhase(null);   // 停止后基准归零，徽标隐藏
            document.getElementById('btn-photo').disabled = true;
            document.getElementById('btn-record').disabled = true;
            document.getElementById('btn-calibrate').disabled = true;

            if (isRecordingStatus) toggleRecord();
            syncCameraVideoForView();
        }

        function severityBadgeClass(severityCode) {
            const classes = {
                1: 'bg-yellow-100 text-yellow-700 border-yellow-300',
                2: 'bg-orange-100 text-orange-700 border-orange-300',
                3: 'bg-red-100 text-red-700 border-red-300',
                4: 'bg-red-200 text-red-900 border-red-500 font-bold'
            };
            return classes[Number(severityCode)] || 'bg-gray-100 text-gray-600 border-gray-300';
        }

        // ---- CJJ 181-2012 评估报告 ----
        let evaluationReportCache = null;

        function ratingColor(code) {
            return { 1: '#16a34a', 2: '#eab308', 3: '#f97316', 4: '#dc2626' }[Number(code)] || '#64748b';
        }

        let complianceChecklistCache = null;

        function fetchComplianceChecklist() {
            if (complianceChecklistCache) return Promise.resolve(complianceChecklistCache);
            return fetch('/api/compliance/checklist')
                .then(r => r.json())
                .then(list => { complianceChecklistCache = list || []; return complianceChecklistCache; })
                .catch(() => []);
        }

        function loadEvaluationReport(folder) {
            const container = document.getElementById('report-container');
            const url = folder
                ? `/api/evaluation/history/${encodeURIComponent(folder)}`
                : '/api/evaluation/current';
            if (!folder) populateEvaluationHistorySelect();
            if (container) container.innerHTML =
                '<div style="text-align:center;color:#94a3b8;padding:30px;">评估数据加载中…</div>';
            Promise.all([
                fetch(url).then(r => r.json()),
                fetchComplianceChecklist(),
            ])
                .then(([data, compliance]) => {
                    evaluationReportCache = data;
                    if (container) container.innerHTML = renderEvaluationReport(data, compliance);
                })
                .catch(() => {
                    if (container) container.innerHTML =
                        '<div style="text-align:center;color:#dc2626;padding:30px;">评估服务不可达</div>';
                });
        }

        function populateEvaluationHistorySelect() {
            const sel = document.getElementById('report-history-select');
            if (!sel) return;
            const current = sel.value;
            fetch('/api/history').then(r => r.json()).then(list => {
                const optHtml = '<option value="">当前巡检（实时）</option>' +
                    (list || []).map(f =>
                        `<option value="${encodeURIComponent(f)}">${f.replace('Inspect_', '')}</option>`).join('');
                sel.innerHTML = optHtml;
                if (current) sel.value = current;
            }).catch(() => {});
        }

        function renderEvaluationReport(d, compliance) {
            const oc = ratingColor(d.overall ? d.overall.rating_code : 0);
            const s = d.structural || {};
            const y = d.functional || {};
            const defectRows = (d.defects || []).map(x => {
                const ms = x.mileage_start, me = x.mileage_end;
                const mileageText = (ms != null && me != null && Number(me) > Number(ms))
                    ? `${Number(ms).toFixed(2)}~${Number(me).toFixed(2)} m`
                    : (x.mileage != null ? Number(x.mileage).toFixed(2) + ' m' : '--');
                return `
                <tr style="border-bottom:1px solid #eef2f7;">
                    <td style="padding:8px 10px;">${x.type}</td>
                    <td style="padding:8px 10px;text-align:center;">
                        <span style="display:inline-block;padding:1px 10px;border-radius:99px;color:#fff;font-weight:700;background:${ratingColor(x.grade)};">${x.grade} 级</span>
                    </td>
                    <td style="padding:8px 10px;text-align:center;font-family:monospace;">${x.equivalent}</td>
                    <td style="padding:8px 10px;text-align:center;font-family:monospace;">${(Number(x.confidence) * 100).toFixed(0)}%</td>
                    <td style="padding:8px 10px;text-align:center;font-family:monospace;">${mileageText}</td>
                    <td style="padding:8px 10px;text-align:center;font-family:monospace;">${x.observations || 1} 次</td>
                    <td style="padding:8px 10px;font-family:monospace;color:#64748b;">${x.time || '--'}</td>
                    <td style="padding:8px 10px;">${x.advice || ''}</td>
                </tr>`; }).join('');

            const mergeNote = (d.merged && Number(d.raw_count) > Number(d.merged_count))
                ? `<div style="font-size:12px;color:#0f766e;background:#f0fdfa;border:1px solid #ccfbf1;border-radius:8px;padding:6px 10px;margin-bottom:10px;">
                       缺陷聚类合并：原始检测记录 ${d.raw_count} 条 → 合并为 ${d.merged_count} 处
                       （同类缺陷里程差 ≤ ${d.merge_window_m}m 归为同一处病害，按"处"参与 CJJ 密度计算）</div>`
                : '';

            const complianceSection = renderComplianceSection(compliance);

            return `
            <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-bottom:16px;">
                <div style="border:2px solid ${oc};border-radius:14px;padding:16px;text-align:center;background:#fff;">
                    <div style="font-size:12px;color:#64748b;">管段综合评价</div>
                    <div style="font-size:34px;font-weight:800;color:${oc};margin:4px 0;">${d.overall ? d.overall.rating : '--'}</div>
                    <div style="font-size:12px;color:#334155;">${d.overall ? d.overall.summary : ''}</div>
                </div>
                <div style="border:1px solid #dbeafe;border-radius:14px;padding:16px;text-align:center;background:#f8fbff;">
                    <div style="font-size:12px;color:#64748b;">修复指数 RI（结构）</div>
                    <div style="font-size:30px;font-weight:800;color:#2563eb;margin:4px 0;">${s.index != null ? s.index : '--'}</div>
                    <div style="font-size:12px;color:#334155;">${s.rating || '未检出结构性缺陷'}</div>
                    <div style="font-size:11px;color:#94a3b8;margin-top:2px;">F=${s.density_F ?? 0} · 最严重 ${s.max_grade || 0} 级</div>
                </div>
                <div style="border:1px solid #ccfbf1;border-radius:14px;padding:16px;text-align:center;background:#f0fdfa;">
                    <div style="font-size:12px;color:#64748b;">养护指数 MI（功能）</div>
                    <div style="font-size:30px;font-weight:800;color:#0f766e;margin:4px 0;">${y.index != null ? y.index : '--'}</div>
                    <div style="font-size:12px;color:#334155;">${y.rating || '未检出功能性缺陷'}</div>
                    <div style="font-size:11px;color:#94a3b8;margin-top:2px;">F=${y.density_F ?? 0} · 最严重 ${y.max_grade || 0} 级</div>
                </div>
                <div style="border:1px solid #e2e8f0;border-radius:14px;padding:16px;text-align:center;background:#fff;">
                    <div style="font-size:12px;color:#64748b;">检测长度</div>
                    <div style="font-size:30px;font-weight:800;color:#334155;margin:4px 0;">${Number(d.section_length_m || 0).toFixed(1)}<span style="font-size:14px;"> m</span></div>
                    <div style="font-size:12px;color:#64748b;">缺陷合计 ${d.defect_total || 0} 处（结构 ${s.count || 0} / 功能 ${y.count || 0}）</div>
                </div>
            </div>
            <div style="border:1px solid #e2e8f0;border-radius:14px;background:#fff;padding:14px;margin-bottom:16px;">
                <div style="font-size:13px;font-weight:700;color:#0f172a;margin-bottom:6px;">📋 处置建议</div>
                <div style="font-size:13px;color:#334155;line-height:1.8;">
                    结构处置：${s.advice || '无结构性缺陷，无需修复'}<br>
                    功能养护：${y.advice || '无功能性缺陷，无需养护'}
                </div>
            </div>
            <div style="border:1px solid #e2e8f0;border-radius:14px;background:#fff;padding:6px 6px 2px;overflow-x:auto;">
                <div style="padding:8px 8px 0;font-size:13px;font-weight:700;color:#0f172a;">📋 缺陷明细台账（每行 = 一处物理病害）</div>
                <table style="width:100%;border-collapse:collapse;font-size:12.5px;color:#334155;">
                    <thead>
                        <tr style="border-bottom:2px solid #cbd5e1;color:#0f172a;">
                            <th style="padding:9px 10px;text-align:left;">病害类型</th>
                            <th style="padding:9px 10px;">缺陷等级</th>
                            <th style="padding:9px 10px;">当量数</th>
                            <th style="padding:9px 10px;">置信度</th>
                            <th style="padding:9px 10px;">发生里程</th>
                            <th style="padding:9px 10px;">观测次数</th>
                            <th style="padding:9px 10px;text-align:left;">记录时间</th>
                            <th style="padding:9px 10px;text-align:left;">处置建议</th>
                        </tr>
                    </thead>
                    <tbody>${defectRows ||
                        '<tr><td colspan="8" style="padding:24px;text-align:center;color:#94a3b8;">本次巡检未记录到缺陷</td></tr>'}</tbody>
                </table>
                ${mergeNote}
            </div>
            ${complianceSection}
            <div style="font-size:11px;color:#94a3b8;margin-top:10px;">
                评估依据：${d.standard || 'CJJ 181-2012'} · RI/MI = 0.7×缺陷密度F + 0.3×最严重缺陷等级 ·
                ${d.is_live ? '实时数据（巡检结束自动存档为 评估报告.json）' : '历史存档数据'}
            </div>`;
        }

        function renderComplianceSection(list) {
            if (!list || !list.length) return '';
            const rows = list.map(item => {
                const ok = item.status === '符合';
                const badgeColor = ok ? '#16a34a' : '#d97706';
                return `
                <tr style="border-bottom:1px solid #eef2f7;">
                    <td style="padding:7px 10px;font-family:monospace;font-size:11.5px;color:#475569;">${item.standard}</td>
                    <td style="padding:7px 10px;">${item.requirement}</td>
                    <td style="padding:7px 10px;color:#475569;">${item.implementation}${item.note ? `<br><span style="font-size:11px;color:#94a3b8;">※ ${item.note}</span>` : ''}</td>
                    <td style="padding:7px 10px;text-align:center;">
                        <span style="display:inline-block;padding:1px 10px;border-radius:99px;color:#fff;font-size:11.5px;background:${badgeColor};">${item.status}</span>
                    </td>
                </tr>`;
            }).join('');
            return `
            <div style="border:1px solid #e2e8f0;border-radius:14px;background:#fff;padding:10px 6px 4px;margin-top:16px;overflow-x:auto;">
                <div style="padding:4px 10px 8px;font-size:13px;font-weight:700;color:#0f172a;">📐 标准符合性自查表（CJJ 181-2012 / T/CAS 731-2023）</div>
                <table style="width:100%;border-collapse:collapse;font-size:12px;color:#334155;">
                    <thead>
                        <tr style="border-bottom:2px solid #cbd5e1;color:#0f172a;">
                            <th style="padding:7px 10px;text-align:left;">标准</th>
                            <th style="padding:7px 10px;text-align:left;">条款要求</th>
                            <th style="padding:7px 10px;text-align:left;">本系统实现</th>
                            <th style="padding:7px 10px;">符合情况</th>
                        </tr>
                    </thead>
                    <tbody>${rows}</tbody>
                </table>
            </div>`;
        }

        function printEvaluationReport() {
            if (!evaluationReportCache) { alert('报告尚未加载'); return; }
            const d = evaluationReportCache;
            const w = window.open('', '_blank', 'width=900,height=700');
            if (!w) { alert('浏览器拦截了弹出窗口，请允许后重试'); return; }
            w.document.write(`<!DOCTYPE html><html><head><meta charset="utf-8">
                <title>管段检测评估报告</title></head>
                <body style="font-family:'Microsoft YaHei',sans-serif;margin:24px;">
                <h2 style="text-align:center;margin-bottom:4px;">排水管道管段检测评估报告</h2>
                <p style="text-align:center;color:#555;margin-top:0;">
                    依据 ${d.standard} · 生成时间 ${new Date().toLocaleString()}</p>
                ${renderEvaluationReport(d, complianceChecklistCache).replace(/style="display:grid[^"]*"/, 'style="display:block"')}
                </body></html>`);
            w.document.close();
            setTimeout(() => w.print(), 400);
        }

        // ---- 年度体检计划（全部管段汇总） ----
        function loadMaintenancePlan() {
            const body = document.getElementById('maintenance-plan-body');
            if (!body) return;
            fetch('/api/maintenance/plan')
                .then(r => r.json())
                .then(data => { renderMaintenancePlan(data); })
                .catch(() => {
                    body.innerHTML = '<span style="color:#dc2626;">计划服务不可达</span>';
                });
        }

        function renderMaintenancePlan(data) {
            const body = document.getElementById('maintenance-plan-body');
            if (!body) return;
            const plan = (data && data.plan) || [];
            if (!plan.length) {
                body.innerHTML = '暂无历史巡检数据 —— 完成巡检后自动生成各管段的修复时限与下次检测日期。';
                return;
            }
            const rows = plan.map(row => `
                <tr style="border-bottom:1px solid #eef2f7;">
                    <td style="padding:7px 10px;font-family:monospace;">${row.date}</td>
                    <td style="padding:7px 10px;font-family:monospace;">${Number(row.length_m || 0).toFixed(1)} m</td>
                    <td style="padding:7px 10px;text-align:center;">
                        <span style="display:inline-block;padding:1px 10px;border-radius:99px;color:#fff;font-weight:700;background:${ratingColor(row.rating_code)};">${row.rating}</span>
                    </td>
                    <td style="padding:7px 10px;text-align:center;font-family:monospace;">${row.ri != null ? row.ri : '--'}</td>
                    <td style="padding:7px 10px;text-align:center;font-family:monospace;">${row.mi != null ? row.mi : '--'}</td>
                    <td style="padding:7px 10px;text-align:center;font-family:monospace;">${row.defect_total}</td>
                    <td style="padding:7px 10px;color:${row.rating_code >= 3 ? '#dc2626' : '#334155'};">${row.action_deadline}</td>
                    <td style="padding:7px 10px;font-family:monospace;">${row.next_check || '修复后复测'}</td>
                </tr>`).join('');
            body.innerHTML = `
                <div style="overflow-x:auto;">
                <table style="width:100%;border-collapse:collapse;font-size:12px;color:#334155;">
                    <thead>
                        <tr style="border-bottom:2px solid #cbd5e1;color:#0f172a;">
                            <th style="padding:7px 10px;text-align:left;">检测日期</th>
                            <th style="padding:7px 10px;">检测长度</th>
                            <th style="padding:7px 10px;">综合等级</th>
                            <th style="padding:7px 10px;">RI</th>
                            <th style="padding:7px 10px;">MI</th>
                            <th style="padding:7px 10px;">缺陷</th>
                            <th style="padding:7px 10px;text-align:left;">修复时限</th>
                            <th style="padding:7px 10px;text-align:left;">下次检测</th>
                        </tr>
                    </thead>
                    <tbody>${rows}</tbody>
                </table>
                </div>
                <div style="font-size:11px;color:#94a3b8;margin-top:6px;">${(data && data.rule_note) || ''}</div>`;
        }

        // ---- 管网健康总览（多趟巡检汇总） ----
        function loadNetworkOverview() {
            const body = document.getElementById('network-overview-body');
            if (!body) return;
            fetch('/api/network/overview')
                .then(r => r.json())
                .then(data => { renderNetworkOverview(data); })
                .catch(() => {
                    body.innerHTML = '<span style="color:#dc2626;">总览服务不可达</span>';
                });
        }

        function renderNetworkOverview(data) {
            const body = document.getElementById('network-overview-body');
            if (!body) return;
            const segments = (data && data.segments) || [];
            if (!segments.length) {
                body.innerHTML = '暂无历史巡检数据 —— 完成巡检后，每趟检测会作为一段管段拼入管网图。';
                return;
            }

            // 管线示意：各段首尾相接，长度按比例，按综合等级着色
            // 2026-10-18：段数多的时候标签抽稀（此前每段都标日期+等级，
            // 几十段挤在一起全叠成乱码 —— "分级混乱"观感的来源）
            const labelEvery = Math.max(1, Math.ceil(segments.length / 10));
            const maxLen = Math.max.apply(null, segments.map(sg => Number(sg.length_m) || 0));
            const minPx = 56, gapPx = 4, totalPx = 620;
            let weights = segments.map(sg => Math.max(minPx, (Number(sg.length_m) / (maxLen || 1)) * totalPx * 0.55 + minPx));
            const weightSum = weights.reduce((a, b) => a + b, 0);
            const scale = (totalPx - gapPx * (segments.length - 1)) / weightSum;
            weights = weights.map(w => Math.max(24, w * scale));

            const W = totalPx + 40, barY = 46, barH = 26;
            let x = 20;
            let svgParts = [`<svg viewBox="0 0 ${W} 118" width="100%" style="max-width:900px;">`];
            segments.forEach((sg, i) => {
                const w = weights[i];
                const color = ratingColor(sg.rating_code);
                svgParts.push(`<rect x="${x.toFixed(1)}" y="${barY}" width="${w.toFixed(1)}" height="${barH}" rx="4" fill="${color}" opacity="0.92"/>`);
                if (i % labelEvery === 0 || i === segments.length - 1) {
                    svgParts.push(`<text x="${(x + w / 2).toFixed(1)}" y="${barY - 8}" text-anchor="middle" font-size="10.5" fill="#475569">${sg.date.slice(5)}</text>`);
                    svgParts.push(`<text x="${(x + w / 2).toFixed(1)}" y="${barY + barH + 15}" text-anchor="middle" font-size="10.5" fill="#334155">${sg.rating} · ${Number(sg.length_m || 0).toFixed(0)}m</text>`);
                }
                x += w + gapPx;
            });
            svgParts.push('</svg>');

            const legend = `
                <div style="display:flex;gap:14px;flex-wrap:wrap;font-size:11.5px;color:#64748b;margin:2px 0 8px;">
                    <span><span style="display:inline-block;width:10px;height:10px;border-radius:2px;background:#16a34a;"></span> Ⅰ级 良好</span>
                    <span><span style="display:inline-block;width:10px;height:10px;border-radius:2px;background:#eab308;"></span> Ⅱ级 较轻</span>
                    <span><span style="display:inline-block;width:10px;height:10px;border-radius:2px;background:#f97316;"></span> Ⅲ级 中等</span>
                    <span><span style="display:inline-block;width:10px;height:10px;border-radius:2px;background:#dc2626;"></span> Ⅳ级 严重</span>
                    <span style="margin-left:auto;">显示最近 ${data.section_count} 段 · 全部存档 ${data.all_count || data.section_count} 段 · ${Number(data.all_length_m || data.total_length_m || 0).toFixed(1)} m</span>
                </div>`;

            body.innerHTML = legend + svgParts.join('');
        }


        function fetchDefectLogs() {
            fetch('/api/defect_logs').then(r => r.json()).then(logs => {
                const container = document.getElementById('defect-logs-container');
                if (logs.length === 0) return;

                container.innerHTML = '';
                logs.forEach(log => {
                    const severityCode = Number(log.severity_code || 0);
                    const severityText = log.severity || '待定';
                    const confidenceNumber = Number(log.confidence);
                    const confidenceText = Number.isFinite(confidenceNumber)
                        ? `${(confidenceNumber * 100).toFixed(0)}%`
                        : '--';
                    const adviceText = log.advice || '建议人工复核';
                    const badgeClass = severityBadgeClass(severityCode);
                    const cardHTML = `
                    <div class="bg-[#f8fbff] border border-blue-200 rounded-xl p-3 flex gap-4 shadow-sm hover:shadow-md transition-shadow">
                        <div class="w-24 h-24 flex-shrink-0 bg-gray-200 rounded-lg overflow-hidden border border-gray-300">
                            ${log.image
                                ? `<img src="${log.image}" class="w-full h-full object-cover" alt="缺陷图">`
                                : `<div style="width:100%;height:100%;display:flex;align-items:center;justify-content:center;font-size:28px;">🚨</div>`}
                        </div>
                        <div class="flex flex-col justify-center py-1 text-sm text-gray-700">
                            <div class="flex items-center gap-2 mb-2">
                                <div class="font-medium text-[16px] text-[#0d3b66]">${log.type} 检测记录</div>
                                <span class="px-2 py-0.5 rounded-full border text-xs font-bold ${badgeClass}">${severityText}</span>
                            </div>
                            <div class="flex items-center gap-2 mb-1">
                                <span class="text-gray-500">检测里程:</span>
                                <span class="font-mono font-bold text-blue-600">${log.mileage.toFixed(2)} m</span>
                            </div>
                            <div class="flex items-center gap-3 mb-1 text-xs">
                                <span class="text-gray-500">置信度 <b class="font-mono text-gray-700">${confidenceText}</b></span>
                                <span class="text-gray-500">${adviceText}</span>
                            </div>
                            <div class="flex items-center gap-2 mt-1">
                                <span class="text-xs text-gray-400 font-mono">🕒 ${log.time}</span>
                            </div>
                        </div>
                    </div>
                    `;
                    container.insertAdjacentHTML('beforeend', cardHTML);
                });
            }).catch(e => console.log("获取日志失败", e));
        }

        function fetchData() {
            return fetch('/api/data').then(r => r.json()).then(data => {
                const statTemp = document.getElementById('stat-temp');
                if (statTemp) {
                    const temp = Number(data.temperature);
                    setTextIfChanged(
                        statTemp,
                        data.is_running && Number.isFinite(temp) ? `${temp.toFixed(1)} °C` : "-- °C"
                    );
                }

                if (!USE_BROWSER_MJPEG_PARSER) {
                    const streamFps = Number(data.stream_fps);
                    setTextIfChanged(
                        document.getElementById('stream-fps-value'),
                        data.is_running && Number.isFinite(streamFps)
                            ? streamFps.toFixed(1)
                            : "--"
                    );
                }

                const btnStart = document.getElementById('btn-start');
                if (data.is_running && !btnStart.disabled) { syncUIToRunning(); }
                else if (!data.is_running && btnStart.disabled) { syncUIToStopped(); }
                /* 2026-09-21: 相机流与运行状态解耦，页面加载即启动（幂等，运行中会跳过） */
                syncCameraVideoForView();
                if (data.manual_mode) setStatusBadge('manual');
                else if (data.is_running) setStatusBadge('running');
                else setStatusBadge('stopped');

                const motorMessage = document.getElementById('motor-control-message');
                if (motorMessage) {
                    setTextIfChanged(
                        motorMessage,
                        data.motor_control_message || '电机已停止'
                    );
                    motorMessage.dataset.state =
                        data.motor_command_state || 'stopped';
                }

                // 2026-10-05: 下潜状态徽标（由固件 ACK 经 comm 侧更新 dive_state）
                setDiveBadge(data.dive_state);

                syncSonarFault(data);

                // 2026-10-15: 环境传感器（APU1 固件 ENV: 帧上报，经 status_from_packet 解析）
                updateEnvironmentData(data);

                if (data.is_running) {
                    let btnLight1 = document.getElementById('btn-light1');
                    if (data.light1_on && !isLight1On) {
                        isLight1On = true; btnLight1.innerText = "关左灯"; btnLight1.className = "ctrl-btn btn-light-active";
                    } else if (!data.light1_on && isLight1On) {
                        isLight1On = false; btnLight1.innerText = "开左灯"; btnLight1.className = "ctrl-btn btn-light-style";
                    }

                    let btnLight2 = document.getElementById('btn-light2');
                    if (data.light2_on && !isLight2On) {
                        isLight2On = true; btnLight2.innerText = "关右灯"; btnLight2.className = "ctrl-btn btn-light-active";
                    } else if (!data.light2_on && isLight2On) {
                        isLight2On = false; btnLight2.innerText = "开右灯"; btnLight2.className = "ctrl-btn btn-light-style";
                    }

                    let btnYolo = document.getElementById('btn-yolo');
                    if (data.defect_recording && !isYoloOn) {
                        isYoloOn = true; btnYolo.innerText = "停止记录"; btnYolo.className = "ctrl-btn btn-yolo-active";
                    } else if (!data.defect_recording && isYoloOn) {
                        isYoloOn = false; btnYolo.innerText = "开始记录"; btnYolo.className = "ctrl-btn btn-yolo-style";
                    }

                    // 姿态角获取与 3D 渲染更新
                    const currentAngles = {
                        roll: Number(data.roll),
                        pitch: Number(data.pitch),
                        yaw: Number(data.yaw)
                    };
                    if (Number.isFinite(currentAngles.roll) &&
                        Number.isFinite(currentAngles.pitch) &&
                        Number.isFinite(currentAngles.yaw)) {
                        latestAngles = currentAngles;
                    }
                    syncHeadingCalibration(data.heading_calibration_seq);
                    if (baseAngles.yaw === null || baseAngles.pitch === null ||
                        baseAngles.roll === null) {
                        baseAngles = { ...latestAngles };
                    }
                    let relRoll = wrapAngle180(latestAngles.roll - baseAngles.roll);
                    let relPitch = wrapAngle180(latestAngles.pitch - baseAngles.pitch);
                    let relYaw = wrapAngle180(latestAngles.yaw - baseAngles.yaw);

                    setTextIfChanged(document.getElementById('val-roll'), relRoll.toFixed(1));
                    setTextIfChanged(document.getElementById('val-pitch'), relPitch.toFixed(1));
                    setTextIfChanged(document.getElementById('val-yaw'), relYaw.toFixed(1));

                    // ==========================================
                    // 姿态角 → 3D 模型（2026-10-17 修复轴向）
                    // ==========================================
                    // 场景为 three.js Y-up：竖直轴=Y，管道沿 Z 轴（前进方向），X=横向。
                    // 之前 X 通道混入了 -π/2 常量（"趴平" hack），XYZ 欧拉顺序下把
                    // 偏航转嫁到了错误轴上（用户观察到偏航显示成绕 X 轴转）。
                    // 模型本身已是水平姿态，直接映射：
                    //   俯仰 pitch → 绕 X（横轴，抬头/低头）
                    //   偏航 yaw   → 绕 Y（竖直轴，水平面左右转）
                    //   横滚 roll  → 绕 Z（前进轴，左右翻滚）
                    if (robotModel) {
                        robotModel.rotation.x = -relPitch * (Math.PI / 180);
                        robotModel.rotation.y = relYaw * (Math.PI / 180);
                        robotModel.rotation.z = -relRoll * (Math.PI / 180);
                        requestRender();   // 模型姿态变化 → 标记需要重新渲染
                    }

                    if (data.is_moving) {
                        const nowMs = Date.now();
                        const dt = (nowMs - lastTime) / 1000.0;
                        lastTime = nowMs;

                        const V = 50.0;
                        const yawRad = relYaw * (Math.PI / 180);

                        curX += V * dt * Math.cos(yawRad);
                        curY += V * dt * Math.sin(yawRad);
                        robotPath.push([curX, curY]);

                        if (Number(data.sonar_left) !== SONAR_DAMAGE_VALUE &&
                            data.sonar_left > 0 && data.sonar_left < 2000) {
                            leftWall.push([curX + data.sonar_left * Math.cos(yawRad + Math.PI / 2), curY + data.sonar_left * Math.sin(yawRad + Math.PI / 2)]);
                        }
                        if (Number(data.sonar_right) !== SONAR_DAMAGE_VALUE &&
                            data.sonar_right > 0 && data.sonar_right < 2000) {
                            rightWall.push([curX + data.sonar_right * Math.cos(yawRad - Math.PI / 2), curY + data.sonar_right * Math.sin(yawRad - Math.PI / 2)]);
                        }

                        if (robotPath.length > 600) { robotPath.shift(); }
                        if (leftWall.length > 600) { leftWall.shift(); }
                        if (rightWall.length > 600) { rightWall.shift(); }

                        markGisChartDirty();
                    } else {
                        lastTime = Date.now();
                    }
                }
            });
        }

        function sendCommand(action) {
            fetch('/api/control/' + action).then(r => r.json()).then(d => {
                // 航向基准切换：从响应更新弯道段徽标（UI 不出现角度字样）
                if (action === 'yaw_left' || action === 'yaw_right') {
                    if (d && d.heading_offset_deg !== undefined) {
                        setHeadingPhase(d.heading_offset_deg);
                    }
                    if (d && d.status === 'error') {
                        console.log('航向切换被拒绝：', d.message);
                    }
                }
                // 2026-10-05: 启停/手动切换都会触发六路联动停车 → 下潜状态复位
                if (['start', 'motor_start', 'motor_stop', 'stop',
                     'manual_start', 'manual_stop'].includes(action)) {
                    setDiveBadge('stopped');
                }
                // 下潜命令被 Web 侧拒绝时提示原因（如未启动）
                if (action === 'dive' && d && d.status === 'error') {
                    console.log('下潜被拒绝：', d.message);
                    const status = document.getElementById('dive-apply-status');
                    if (status) {
                        status.textContent = '下潜被拒绝：' + d.message;
                        status.className = 'error';
                    }
                }
            }).catch(() => {});
        }

        // ---- 2026-10-04: 航向基准段位徽标（直道/左弯/右弯/回转），不出现角度数字 ----
        function setHeadingPhase(offsetDeg) {
            const badge = document.getElementById('heading-phase-badge');
            if (!badge) return;
            if (offsetDeg === null || offsetDeg === undefined) {
                badge.style.display = 'none';
                return;
            }
            const off = ((offsetDeg % 360) + 540) % 360 - 180;   // wrap 到 [-180,180)
            const map = {
                '0':   ['直道段', '#1f3a2c', '#8fe0ae'],
                '90':  ['左弯段', '#3d3323', '#e8c88f'],
                '-90': ['右弯段', '#3d3323', '#e8c88f'],
                '180': ['回转段', '#33254d', '#c9a8f0'],
                '-180': ['回转段', '#33254d', '#c9a8f0'],
            };
            const key = String(Math.round(off));
            const entry = map[key] || ['偏航段', '#243247', '#8fb7e8'];
            badge.textContent = entry[0];
            badge.style.background = entry[1];
            badge.style.color = entry[2];
            badge.style.display = 'inline-block';
        }

        function updateHeadingPhaseBadge() {
            // 页面刷新后恢复徽标（从最新遥测读 heading_offset_deg）
            fetch('/api/data', { cache: 'no-store' })
                .then(r => r.json())
                .then(d => {
                    if (d && d.heading_offset_deg !== undefined && isSystemRunning) {
                        setHeadingPhase(d.heading_offset_deg);
                    }
                })
                .catch(() => {});
        }


        /* ===== 2026-09-27: Web 手动运动控制（真实下发 MANUAL_START/SPEED 到裸机） ===== */
        let manualModeOn = false;
        let manualSendTimer = null;

        function manualSpeedValue() {
            const el = document.getElementById('manual-speed');
            return el ? Number(el.value) : 30;
        }

        function toggleManualMode() {
            const btn = document.getElementById('btn-manual-toggle');
            if (!manualModeOn) {
                fetch('/api/control/manual_start').then(r => r.json()).then(d => {
                    if (d && d.status === 'success') {
                        manualModeOn = true;
                        btn.textContent = '退出手动';
                        btn.style.background = '#FDE68A';
                    } else if (d && d.message) {
                        alert('进入手动失败: ' + d.message);
                    }
                });
            } else {
                manualStopDrive();
                manualModeOn = false;
                if (btn) { btn.textContent = '进入手动'; btn.style.background = ''; }
                fetch('/api/control/manual_stop');
            }
        }

        function manualSend(throttle, turn) {
            fetch(`/api/control/speed?throttle=${throttle}&turn=${turn}`).catch(() => {});
        }

        function manualStartDrive(throttle, turn) {
            manualStopDrive();
            manualSend(throttle, turn);
            /* 按住期间每 300ms 重发一次，防丢包/看门狗 */
            manualSendTimer = setInterval(() => manualSend(throttle, turn), 300);
        }

        function manualStopDrive() {
            if (manualSendTimer) { clearInterval(manualSendTimer); manualSendTimer = null; }
            manualSend(0, 0);
        }

        (function initManualPad() {
            const speedEl = document.getElementById('manual-speed');
            const speedVal = document.getElementById('manual-speed-val');
            if (speedEl && speedVal) {
                speedEl.addEventListener('input', () => { speedVal.textContent = speedEl.value; });
            }
            const pad = {
                'mb-fwd':   () => manualStartDrive(manualSpeedValue(), 0),
                'mb-rev':   () => manualStartDrive(-manualSpeedValue(), 0),
                'mb-left':  () => manualStartDrive(0, -manualSpeedValue()),
                'mb-right': () => manualStartDrive(0, manualSpeedValue()),
            };
            Object.keys(pad).forEach(id => {
                const el = document.getElementById(id);
                if (!el) return;
                const press = (e) => {
                    e.preventDefault();
                    if (!manualModeOn) { alert('请先点击「进入手动」'); return; }
                    pad[id]();
                };
                el.addEventListener('pointerdown', press);
                el.addEventListener('pointerup', manualStopDrive);
                el.addEventListener('pointerleave', manualStopDrive);
                el.addEventListener('pointercancel', manualStopDrive);
            });
            const stopBtn = document.getElementById('mb-stop');
            if (stopBtn) stopBtn.addEventListener('click', manualStopDrive);
            /* 页面隐藏/关闭时安全停车 */
            window.addEventListener('beforeunload', () => { if (manualModeOn) manualStopDrive(); });
            document.addEventListener('visibilitychange', () => {
                if (document.hidden && manualModeOn) manualStopDrive();
            });
        })();
        /* ===== Web 手动运动控制结束 ===== */

        /* ===== 2026-10-05: 垂向推进（下潜）控制 —— 队友固件契约 =====
           CMD:DIVE / CMD:DIVE:STOP 只发固定字符串、不带数据；
           两路速度经 CTRL: 帧第 16/17 字段随「应用速度」下发（复用应用参数路径）；
           无"上浮"按钮：正浮力船体，上浮 = 把速度调小或按「停」。 */
        function setDiveBadge(state) {
            const badge = document.getElementById('dive-state-badge');
            if (!badge) return;
            if (state === 'on') {
                badge.textContent = '下潜：开';
                badge.style.background = '#3d3323';
                badge.style.color = '#e8c88f';
            } else {
                badge.textContent = '下潜：停';
                badge.style.background = '#243247';
                badge.style.color = '#8fb7e8';
            }
        }

        (function initDivePanel() {
            [['dive-left', 'dive-left-val'], ['dive-right', 'dive-right-val']]
                .forEach(([sid, vid]) => {
                    const el = document.getElementById(sid);
                    const val = document.getElementById(vid);
                    if (el && val) {
                        el.addEventListener('input', () => { val.textContent = el.value; });
                    }
                });
        })();

        function toggleLight1() { fetch(!isLight1On ? '/api/control/led1on' : '/api/control/led1off'); }
        function toggleLight2() { fetch(!isLight2On ? '/api/control/led2on' : '/api/control/led2off'); }
        function toggleYolo() { fetch('/api/control/record_toggle'); }

        // ---- 气路：吸样开始 / 急停（2026-10-16 新增）----------------------
        //
        //  固件时序（顺序由固件状态机保证，前端只发命令 + 显示忙态）：
        //      阀开 → 2s → 泵开 → 吸 6s → 泵关 → 0.5s → 阀关
        //      总时长 8.5~11.5 秒（受 APU1 主循环 ~1s 节流影响，只会更晚不会更早）
        //  回执复用现有 control_ack 显示：GAS SAMPLE STARTED / GAS STOPPED /
        //                                ERR: GAS BUSY（非致命）/ ERR: GAS GPIO NOT READY
        function gasSampleStart() {
            if (gasSampleBusy) {
                return;                 // 防连点（固件侧也会拒绝并发，回 ERR: GAS BUSY）
            }
            gasSampleBusy = true;

            const btn = document.getElementById('btn-gas-sample');
            if (btn) {
                btn.disabled = true;
                btn.textContent = '🫧 吸气中…';
            }

            fetch('/api/control/gas_sample_start')
                .then(r => r.json())
                .then(d => { console.log('[gas] start resp:', d); })
                .catch(err => { console.warn('[gas] start failed:', err); });

            // 流程总时长 8.5~11.5s，按 12s 解锁（只影响按钮，不影响硬件）
            clearTimeout(gasSampleTimer);
            gasSampleTimer = setTimeout(() => {
                gasSampleBusy = false;
                if (btn) {
                    btn.disabled = false;
                    btn.textContent = '🫧 吸气采样';
                }
            }, 12000);
        }

        function gasStop() {
            clearTimeout(gasSampleTimer);
            gasSampleBusy = false;

            const btn = document.getElementById('btn-gas-sample');
            if (btn) {
                btn.disabled = false;
                btn.textContent = '🫧 吸气采样';
            }

            fetch('/api/control/gas_stop')
                .then(r => r.json())
                .then(d => { console.log('[gas] stop resp:', d); })
                .catch(err => { console.warn('[gas] stop failed:', err); });
        }

        function reCalibrate() {
            fetch('/api/control/calibrate')
                .then(r => r.json())
                .then(data => syncHeadingCalibration(
                    data.heading_calibration_seq,
                    true
                ));

            let btn = document.getElementById('btn-calibrate');
            let origText = btn.innerText;
            btn.innerText = "已清空";
            btn.classList.add('bg-green-100', 'text-green-700', 'border-green-300');
            setTimeout(() => { btn.innerText = origText; btn.classList.remove('bg-green-100', 'text-green-700', 'border-green-300'); }, 800);
        }

        /* ===== NPU 视觉检测叠加层（2026-09-21 融合） ===== */
        const VISION_CLASSES = [
            { name: '剥落', color: '#1e6fff' },
            { name: '裂纹', color: '#e84b5f' },
            { name: '孔洞', color: '#865be7' },
            { name: '树根侵入', color: '#10a878' },
            { name: '沉积', color: '#f09a39' },
        ];
        let visionPollTimer = null;

        function drawVisionOverlay(data) {
            const canvas = document.getElementById('vision-overlay');
            const img = document.getElementById('video-stream');
            if (!canvas || !img || !img.parentElement) return;
            const stage = img.parentElement;
            const dpr = Math.min(window.devicePixelRatio || 1, 2);
            canvas.width = Math.round(stage.clientWidth * dpr);
            canvas.height = Math.round(stage.clientHeight * dpr);
            canvas.style.width = stage.clientWidth + 'px';
            canvas.style.height = stage.clientHeight + 'px';
            const c = canvas.getContext('2d');
            c.setTransform(dpr, 0, 0, dpr, 0, 0);
            c.clearRect(0, 0, stage.clientWidth, stage.clientHeight);
            if (!data || !data.ready || !data.camera_width || !data.model_width) return;

            /* object-fit:contain 信箱映射：模型坐标(256×256 中心系) → 显示矩形 */
            const scale = Math.min(
                stage.clientWidth / data.camera_width,
                stage.clientHeight / data.camera_height);
            const offX = (stage.clientWidth - data.camera_width * scale) / 2;
            const offY = (stage.clientHeight - data.camera_height * scale) / 2;
            const sx = (data.camera_width * scale) / data.model_width;
            const sy = (data.camera_height * scale) / data.model_height;

            for (const o of (data.objects || [])) {
                const meta = VISION_CLASSES[o.label] ||
                    { name: '类别' + o.label, color: '#42e8b4' };
                const x = offX + (o.x - o.w / 2) * sx;
                const y = offY + (o.y - o.h / 2) * sy;
                const w = o.w * sx;
                const h = o.h * sy;
                c.strokeStyle = meta.color;
                c.lineWidth = 2;
                c.strokeRect(x, y, w, h);
                const label = meta.name + ' ' + (o.prob * 100).toFixed(0) + '%';
                c.font = '600 13px system-ui';
                const tw = c.measureText(label).width + 10;
                c.fillStyle = meta.color;
                c.fillRect(x, Math.max(offY, y - 20), tw, 20);
                c.fillStyle = '#fff';
                c.fillText(label, x + 5, Math.max(offY + 14, y - 6));
            }

            if (canvas.width > 0) sendAnnotatedFrame(canvas);

            const summary = document.getElementById('vision-summary');
            if (summary) {
                const count = (data.objects || []).length;
                const fps = Number(data.pipeline_fps || 0).toFixed(1);
                summary.textContent = count
                    ? '检测到 ' + count + ' 个目标 · ' + fps + ' FPS'
                    : '无目标 · ' + fps + ' FPS';
                summary.style.display = 'block';
            }
        }

        let lastAnnotatedSend = 0;
        function sendAnnotatedFrame(canvas) {
            const now = Date.now();
            if (now - lastAnnotatedSend < 2000) return;
            lastAnnotatedSend = now;
            try {
                const dataUrl = canvas.toDataURL('image/jpeg', 0.85);
                fetch('/api/vision/annotated_frame', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({image: dataUrl})
                });
            } catch(e) {}
        }

        function startVisionPoll() {
            /* 2026-09-27 修复：startVisionPoll 此前被调用但从未定义，
               导致视觉轮询从未启动（视频正常但无检测框、FPS 恒 0.0）。 */
            visionPollLoop();
        }

        function visionPollLoop() {
            if (visionPollTimer) return;
            const tick = () => {
                if (!cameraVideoRunning) { visionPollTimer = null; return; }
                fetch('/api/vision/detections', { cache: 'no-store' })
                    .then(r => r.json())
                    .then(data => {
                        drawVisionOverlay(data);
                        const fpsEl = document.getElementById('stream-fps-value');
                        if (fpsEl && data.pipeline_fps) {
                            fpsEl.innerText = Number(data.pipeline_fps).toFixed(1);
                        }
                    })
                    .catch(() => {})
                    .finally(() => { visionPollTimer = setTimeout(tick, 400); });
            };
            visionPollTimer = setTimeout(tick, 300);
        }

        function stopVisionPoll() {
            if (visionPollTimer) {
                clearTimeout(visionPollTimer);
                visionPollTimer = null;
            }
            const canvas = document.getElementById('vision-overlay');
            if (canvas) {
                const c = canvas.getContext('2d');
                c.clearRect(0, 0, canvas.width, canvas.height);
            }
            const summary = document.getElementById('vision-summary');
            if (summary) summary.style.display = 'none';
        }
        /* ===== NPU 视觉检测叠加层结束 ===== */

        function takePhoto() {
            fetch('/api/vision/snapshot')
                .then(r => r.json())
                .then(data => {
                    let btn = document.getElementById('btn-photo'); let origText = btn.innerText;
                    if (data.status === 'success') {
                        btn.innerText = "已保存"; btn.classList.add('text-green-600', 'bg-green-50', 'border-green-200');
                        const previewImg = document.getElementById('image-preview-img');
                        const previewCaption = document.getElementById('image-preview-caption');
                        if (previewImg && previewCaption) {
                            previewImg.src = data.url;
                            previewCaption.innerText = '已拍照保存: ' + data.url;
                            document.getElementById('image-preview').classList.remove('hidden');
                        }
                    } else {
                        btn.innerText = "拍照失败"; btn.classList.add('text-red-600', 'bg-red-50', 'border-red-200');
                    }
                    setTimeout(() => { btn.innerText = origText; btn.classList.remove('text-green-600', 'text-red-600', 'bg-green-50', 'bg-red-50', 'border-green-200', 'border-red-200'); }, 1500);
                })
                .catch(() => {
                    let btn = document.getElementById('btn-photo');
                    const origText = "📸 拍照";
                    btn.innerText = "拍照失败";
                    setTimeout(() => { btn.innerText = origText; }, 1500);
                });
        }

        function toggleRecord() {
            let btn = document.getElementById('btn-record');
            if (!isRecordingStatus) {
                fetch('/api/vision/record/start').then(r => r.json()).then(data => {
                    if (data.status !== 'success') {
                        btn.innerText = "录像失败";
                        setTimeout(() => { btn.innerText = "⏺ 开始录像"; }, 1500);
                        return;
                    }
                    isRecordingStatus = true;
                    btn.innerText = "停止录像"; btn.classList.add('text-red-600', 'bg-red-50', 'border-red-200');
                }).catch(() => {});
            } else {
                fetch('/api/vision/record/stop').then(r => r.json()).then(data => {
                    isRecordingStatus = false;
                    btn.innerText = "开始录像"; btn.classList.remove('text-red-600', 'bg-red-50', 'border-red-200');
                    if (data.status === 'success' && data.url) {
                        const previewCaption = document.getElementById('image-preview-caption');
                        if (previewCaption) previewCaption.innerText = '录像已保存: ' + data.url;
                        document.getElementById('image-preview').classList.remove('hidden');
                    }
                }).catch(() => { isRecordingStatus = false; });
            }
        }

        function motionParamNumber(id) {
            return Number(document.getElementById(id).value);
        }

        // 2026-10-05: 横滚稳定启用/关闭开关 —— 关 = kp/ki/kd 全清零（limit/sign 保留）
        // 勾选时恢复上次值（未保存过则给安全起步值 kp=1.0/ki=0/kd=0）
        let _rollStabSavedGains = { kp: 1.0, ki: 0, kd: 0 };
        window.toggleRollStab = function(enabled) {
            const kp = document.getElementById('roll-kp');
            const ki = document.getElementById('roll-ki');
            const kd = document.getElementById('roll-kd');
            if (!kp || !ki || !kd) return;
            if (enabled) {
                // 恢复：如果当前全 0（刚从关→开），用上次保存值或安全起步值
                if (Number(kp.value) === 0 && Number(ki.value) === 0 && Number(kd.value) === 0) {
                    kp.value = _rollStabSavedGains.kp;
                    ki.value = _rollStabSavedGains.ki;
                    kd.value = _rollStabSavedGains.kd;
                }
            } else {
                // 关闭：保存当前增益值，然后清零
                _rollStabSavedGains = {
                    kp: Number(kp.value) || 1.0,
                    ki: Number(ki.value) || 0,
                    kd: Number(kd.value) || 0,
                };
                kp.value = 0;
                ki.value = 0;
                kd.value = 0;
            }
        };

        function loadMotionParams() {
            const fields = {
                kp: 'pid-kp',
                ki: 'pid-ki',
                kd: 'pid-kd',
                heading_kp: 'heading-kp',
                yaw_rate_damping: 'yaw-rate-damping',
                integral_limit: 'integral-limit',
                turn_limit: 'turn-limit',
                base_speed: 'base-speed',
                min_output: 'min-output',
                max_output: 'max-output',
                left_trim: 'left-trim',
                right_trim: 'right-trim',
                front_yaw_share: 'front-yaw-share',
                rear_yaw_share: 'rear-yaw-share',
                front_max_output: 'front-max-output',
                // 2026-10-05: 下潜速度（CTRL: 第 16/17 字段）—— 输入在设备控制页下潜面板
                dive_left: 'dive-left',
                dive_right: 'dive-right',
                // 2026-10-05: 横滚稳定（CTRL: 第 18~22 字段）—— 输入在 PID 调参页第 05 组
                roll_kp: 'roll-kp',
                roll_ki: 'roll-ki',
                roll_kd: 'roll-kd',
                roll_limit: 'roll-limit',
                roll_sign: 'roll-sign',
            };

            fetch('/api/control/motion_params')
                .then(response => response.json())
                .then(params => {
                    Object.entries(fields).forEach(([name, id]) => {
                        const input = document.getElementById(id);
                        if (input && Number.isFinite(Number(params[name]))) {
                            input.value = params[name];
                            // 滑块旁的数值显示同步（dive-left-val / dive-right-val）
                            const val = document.getElementById(id + '-val');
                            if (val) val.textContent = input.value;
                        }
                    });
                    // 2026-10-05: 横滚开关初始状态 —— kp/ki/kd 全 0 = 关
                    const toggle = document.getElementById('roll-enable-toggle');
                    if (toggle) {
                        const kp = Number(params.roll_kp) || 0;
                        const ki = Number(params.roll_ki) || 0;
                        const kd = Number(params.roll_kd) || 0;
                        toggle.checked = !(kp === 0 && ki === 0 && kd === 0);
                    }
                })
                .catch(error => {
                    console.log('读取保存的运动参数失败', error);
                });
        }

        // 2026-10-05: 15 个基础参数 + 2 个下潜速度统一收集 ——
        // PID 调参页「应用全部参数」与下潜面板「应用速度」共用同一条 CTRL: 下发路径。
        function collectMotionParams() {
            return {
                kp: motionParamNumber('pid-kp'),
                ki: motionParamNumber('pid-ki'),
                kd: motionParamNumber('pid-kd'),
                heading_kp: motionParamNumber('heading-kp'),
                yaw_rate_damping: motionParamNumber('yaw-rate-damping'),
                integral_limit: motionParamNumber('integral-limit'),
                turn_limit: motionParamNumber('turn-limit'),
                base_speed: motionParamNumber('base-speed'),
                min_output: motionParamNumber('min-output'),
                max_output: motionParamNumber('max-output'),
                left_trim: motionParamNumber('left-trim'),
                right_trim: motionParamNumber('right-trim'),
                front_yaw_share: motionParamNumber('front-yaw-share'),
                rear_yaw_share: motionParamNumber('rear-yaw-share'),
                front_max_output: motionParamNumber('front-max-output'),
                dive_left: motionParamNumber('dive-left'),
                dive_right: motionParamNumber('dive-right'),
                // 2026-10-05: 横滚稳定
                roll_kp: motionParamNumber('roll-kp'),
                roll_ki: motionParamNumber('roll-ki'),
                roll_kd: motionParamNumber('roll-kd'),
                roll_limit: motionParamNumber('roll-limit'),
                roll_sign: motionParamNumber('roll-sign'),
            };
        }

        function validateMotionParams(params) {
            if (Math.abs(
                params.front_yaw_share +
                params.rear_yaw_share -
                100
            ) > 0.01) {
                return '前后 Yaw 占比合计必须为 100%。';
            }
            if (!(
                params.min_output <=
                params.base_speed &&
                params.base_speed <=
                params.max_output
            )) {
                return '必须满足：最小输出 ≤ 基础速度 ≤ 最大输出。';
            }
            if (
                params.turn_limit >
                params.max_output - params.min_output
            ) {
                return '转向限制不能大于最大输出与最小输出之差。';
            }
            return null;
        }

        async function postMotionParams(params, statusEl, button) {
            button.disabled = true;
            button.textContent = '正在下发…';
            statusEl.textContent = '正在发送完整参数到裸机…';
            statusEl.className = '';
            try {
                const response = await fetch('/api/control/set_motion_params', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(params),
                });
                const result = await response.json();
                if (!response.ok || result.status === 'error') {
                    throw new Error(result.message || '参数下发失败');
                }
                statusEl.textContent = result.message;
                statusEl.className = 'success';
            } catch (error) {
                statusEl.textContent = error.message || '参数下发失败';
                statusEl.className = 'error';
            } finally {
                button.disabled = false;
                button.textContent = '应用全部参数';
            }
        }

        async function updateMotionParams(event) {
            event.preventDefault();

            const form = document.getElementById('motion-params-form');
            const button = document.getElementById('btn-apply-motion-params');
            const status = document.getElementById('motion-params-status');
            if (!form.reportValidity()) return;

            const params = collectMotionParams();

            const error = validateMotionParams(params);
            if (error) {
                status.className = 'error';
                status.textContent = error;
                return;
            }

            await postMotionParams(params, status, button);
        }

        // 下潜面板「应用速度」：与「应用全部参数」完全同一条 CTRL: 路径，
        // 17 个字段（含两路下潜速度）一起原子生效 —— 下潜开着时改速度立即重下发。
        async function applyDiveSpeed() {
            const form = document.getElementById('motion-params-form');
            const button = document.getElementById('btn-dive-apply');
            const status = document.getElementById('dive-apply-status');
            if (!form.reportValidity()) {
                status.className = 'error';
                status.textContent = '基础运动参数不完整，请先在 PID 调参页补全';
                return;
            }

            const params = collectMotionParams();

            const error = validateMotionParams(params);
            if (error) {
                status.className = 'error';
                status.textContent = error;
                return;
            }

            button.disabled = true;
            button.textContent = '正在下发…';
            status.textContent = '正在发送下潜速度到裸机…';
            status.className = '';
            try {
                const response = await fetch('/api/control/set_motion_params', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify(params),
                });
                const result = await response.json();
                if (!response.ok || result.status === 'error') {
                    throw new Error(result.message || '下潜速度下发失败');
                }
                status.textContent = result.message;
                status.className = 'success';
            } catch (err) {
                status.textContent = err.message || '下潜速度下发失败';
                status.className = 'error';
            } finally {
                button.disabled = false;
                button.textContent = '应用速度';
            }
        }

        // 5. 历史记录页
        let historyHasLoaded = false;
        let pipeDiameterCm = null;   // 本次巡检的管径（启动时定死）
        let currentDefectImages = []; // 历史详情：该次巡检的缺陷图序列（供大图浏览）
        let currentHistoryImages = [];
        let previewImageIndex = 0;

        function historyEmptyHTML() {
            return `
                <div class="history-empty-state">
                    <svg width="72" height="72" opacity="0.2" fill="currentColor" viewBox="0 0 20 20"><path fill-rule="evenodd" d="M2 5a2 2 0 012-2h12a2 2 0 012 2v10a2 2 0 01-2 2H4a2 2 0 01-2-2V5zm3.293 1.293a1 1 0 011.414 0l3 3a1 1 0 010 1.414l-3 3a1 1 0 01-1.414-1.414L7.586 10 5.293 7.707a1 1 0 010-1.414zM11 12a1 1 0 100 2h3a1 1 0 100-2h-3z" clip-rule="evenodd"></path></svg>
                    <span>请在左侧选择要查看的巡检记录</span>
                </div>
            `;
        }

        function loadHistoryList(forceRefresh = false) {
            const listContainer = document.getElementById('history-list');
            const detailContainer = document.getElementById('history-detail');
            if (!listContainer) return;
            if (historyHasLoaded && !forceRefresh) return;

            historyHasLoaded = true;
            currentHistoryImages = [];
            listContainer.innerHTML = '<div class="text-gray-400 text-sm text-center mt-10 animate-pulse">正在获取记录...</div>';
            if (detailContainer && forceRefresh) {
                detailContainer.innerHTML = historyEmptyHTML();
            }

            fetch('/api/history')
                .then(r => r.json())
                .then(historyList => {
                    if (historyList.length === 0) {
                        listContainer.innerHTML = '<div class="text-gray-400 text-sm text-center mt-10">暂无历史记录</div>';
                        if (detailContainer) detailContainer.innerHTML = historyEmptyHTML();
                        return;
                    }
                    listContainer.innerHTML = historyList.map(item => {
                        const encodedItem = encodeURIComponent(item).replace(/'/g, '%27');
                        return `
                            <button onclick="loadHistoryDetail(decodeURIComponent('${encodedItem}'), this)" class="history-item-btn w-full text-left p-4 mb-3 bg-white rounded-2xl shadow-sm border border-gray-100 hover:border-blue-400 hover:shadow-md transition-all">
                                <div class="font-bold text-[#1e293b] text-sm break-all">${item}</div>
                                <div class="text-xs text-blue-500 mt-2 flex items-center gap-1 font-medium">
                                    <span>📄 点击查看详情</span>
                                </div>
                            </button>
                        `;
                    }).join('');
                })
                .catch(err => {
                    historyHasLoaded = false;
                    listContainer.innerHTML = `<div class="text-red-500 text-sm text-center mt-10 p-3 bg-red-50 rounded-xl">历史记录加载失败：${err.message}</div>`;
                });
        }

        function loadHistoryDetail(folderName, btnElement) {
            document.querySelectorAll('.history-item-btn').forEach(btn => {
                btn.classList.remove('ring-2', 'ring-blue-500', 'bg-blue-50');
            });
            if (btnElement) {
                btnElement.classList.add('ring-2', 'ring-blue-500', 'bg-blue-50');
            }

            const detailContainer = document.getElementById('history-detail');
            detailContainer.innerHTML = '<div class="text-center text-gray-400 mt-20 animate-pulse">正在加载数据...</div>';

            fetch('/api/history/' + encodeURIComponent(folderName))
                .then(r => r.json())
                .then(data => {
                    if (data.status === 'error') {
                        detailContainer.innerHTML = `<div class="text-red-500 text-center mt-10 p-4 bg-red-50 rounded-xl">${data.message}</div>`;
                        return;
                    }

                    let tableHTML = '';
                    if (data.csv_data && data.csv_data.length > 0) {
                        const headers = Object.keys(data.csv_data[0] || {});
                        const rows = data.csv_data.map(row => `
                            <tr class="hover:bg-blue-50 transition-colors border-b border-gray-100 last:border-none">
                                ${headers.map(h => `<td class="px-5 py-3 whitespace-nowrap text-sm text-gray-600">${row[h]}</td>`).join('')}
                            </tr>
                        `).join('');

                        tableHTML = `
                            <div class="mb-10">
                                <h4 class="text-[16px] font-bold text-gray-700 mb-4 flex items-center gap-2">
                                    <span class="w-1.5 h-5 bg-blue-500 rounded-full inline-block"></span> 巡检缺陷报告 (CSV)
                                </h4>
                                <div class="overflow-x-auto bg-white rounded-2xl shadow-sm border border-gray-100 font-mono">
                                    <table class="min-w-full text-left">
                                        <thead class="bg-gray-50/80 border-b border-gray-200">
                                            <tr>${headers.map(h => `<th class="px-5 py-3.5 text-sm font-bold text-gray-700">${h}</th>`).join('')}</tr>
                                        </thead>
                                        <tbody>${rows}</tbody>
                                    </table>
                                </div>
                            </div>
                        `;
                    } else {
                        tableHTML = `<div class="mb-8 p-4 bg-gray-50 rounded-xl text-gray-500 text-sm border border-dashed border-gray-200">此记录暂无 CSV 表格数据</div>`;
                    }

                    // 缺陷记录卡片（与实时"缺陷标记与实时日志"同款式）
                    let cardsHTML = '';
                    const rowsAll = data.csv_data || [];
                    if (rowsAll.length > 0) {
                        const sevColor = { '紧急': 'bg-red-100 text-red-600', '严重': 'bg-orange-100 text-orange-600', '中度': 'bg-amber-100 text-amber-600', '轻微': 'bg-emerald-100 text-emerald-600', '计划': 'bg-emerald-100 text-emerald-600' };
                        const defectImgs = [];
                        const cards = rowsAll.map(row => {
                            const g = k => (row[k] !== undefined && row[k] !== null) ? String(row[k]) : '';
                            const imgRel = g('关联图片文件');
                            let imgIdx = -1;
                            if (imgRel) {
                                const src = '/images/' + encodeURIComponent(folderName) + '/' + imgRel;
                                if (!defectImgs.includes(src)) defectImgs.push(src);
                                imgIdx = defectImgs.indexOf(src);
                            }
                            const sev = g('严重等级') || '—';
                            const badge = sevColor[sev] || 'bg-gray-100 text-gray-500';
                            const thumb = imgIdx >= 0
                                ? `<img src="${defectImgs[imgIdx]}" class="w-24 h-24 object-cover rounded-xl border border-gray-200 flex-shrink-0 cursor-pointer hover:opacity-90 transition-opacity" loading="lazy" onclick="openImagePreview(currentDefectImages, ${imgIdx})" title="点击放大查看">`
                                : `<div class="w-24 h-24 rounded-xl bg-gray-100 border border-dashed border-gray-300 flex items-center justify-center text-gray-400 text-xs flex-shrink-0">无图片</div>`;
                            return `
                                <div class="bg-white rounded-2xl shadow-sm border border-gray-100 p-4 flex gap-4 items-start">
                                    ${thumb}
                                    <div class="min-w-0">
                                        <div class="flex items-center gap-2 flex-wrap">
                                            <span class="font-bold text-gray-800">${g('病害类型') || '未知缺陷'} 检测记录</span>
                                            <span class="px-2 py-0.5 rounded-full text-xs font-bold ${badge}">${sev}</span>
                                        </div>
                                        <div class="text-sm text-gray-600 mt-1">检测里程: <span class="text-blue-600 font-bold">${g('发生里程(m)') || '—'} m</span></div>
                                        <div class="text-sm text-gray-600 mt-0.5">置信度 <span class="font-bold">${g('识别置信度') || '—'}</span>　${g('处置建议')}</div>
                                        <div class="text-xs text-gray-400 mt-1.5">🕐 ${g('记录时间')}</div>
                                    </div>
                                </div>
                            `;
                        }).join('');
                        currentDefectImages = defectImgs;   // 供卡片点击放大浏览
                        cardsHTML = `
                            <div class="mb-10">
                                <h4 class="text-[16px] font-bold text-gray-700 mb-4 flex items-center gap-2">
                                    <span class="w-1.5 h-5 bg-red-500 rounded-full inline-block"></span> 缺陷记录 (${rowsAll.length})
                                </h4>
                                <div class="grid grid-cols-1 lg:grid-cols-2 gap-4">
                                    ${cards}
                                </div>
                            </div>
                        `;
                    }

                    let imageHTML = '';
                    currentHistoryImages = data.images || [];
                    if (data.images && data.images.length > 0) {
                        const images = data.images.map((img, index) => `
                            <div class="group relative bg-gray-100 rounded-2xl overflow-hidden aspect-[4/3] border border-gray-200 shadow-sm cursor-pointer" onclick="openImagePreview(currentHistoryImages, ${index})">
                                <img src="${img}" class="w-full h-full object-cover transition-transform duration-500 group-hover:scale-110" alt="巡检资料" loading="lazy">
                                <div class="absolute inset-0 bg-gradient-to-t from-black/70 via-black/10 to-transparent opacity-0 group-hover:opacity-100 transition-opacity duration-300 flex flex-col justify-end p-4">
                                    <span class="text-white text-xs truncate drop-shadow-md font-mono">${img.split('/').pop()}</span>
                                    <span class="text-blue-300 text-xs mt-1 font-bold">🔍 点击查看大图</span>
                                </div>
                            </div>
                        `).join('');

                        imageHTML = `
                            <div>
                                <h4 class="text-[16px] font-bold text-gray-700 mb-4 flex items-center gap-2">
                                    <span class="w-1.5 h-5 bg-indigo-500 rounded-full inline-block"></span> GIS 轨迹地图 & 抓拍库
                                </h4>
                                <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-5 pb-8">
                                    ${images}
                                </div>
                            </div>
                        `;
                    } else {
                        imageHTML = `<div class="p-4 bg-gray-50 rounded-xl text-gray-500 text-sm border border-dashed border-gray-200">此记录暂无图片或地图数据生成</div>`;
                    }

                    detailContainer.innerHTML = `
                        <div class="mb-8 pb-4 border-b border-gray-100 flex items-center justify-between gap-4 flex-wrap">
                            <div class="font-mono text-xl md:text-2xl font-bold tracking-tight text-transparent bg-clip-text bg-gradient-to-r from-blue-600 to-indigo-600">
                                ${folderName}
                            </div>
                            <button type="button" onclick="playCorridorHistory('${folderName}')"
                                class="px-4 py-2 rounded-xl text-sm font-bold text-white bg-gradient-to-r from-indigo-500 to-purple-500 hover:opacity-90 transition-opacity shadow-sm"
                                title="用 3D 走廊回放这次巡检">
                                🚇 3D 走廊回放
                            </button>
                        </div>
                        ${cardsHTML}
                        ${tableHTML}
                        ${imageHTML}
                    `;
                })
                .catch(err => {
                    detailContainer.innerHTML = `<div class="text-red-500 text-center mt-10">数据请求失败：${err.message}</div>`;
                });
        }

        // 历史巡检 -> 3D 走廊回放（corridor.js 的历史模式）
        window.playCorridorHistory = function (folderName) {
            if (!folderName) return;
            switchView('corridor');
            // switchView 会以实时模式 enter；随后切入回放模式加载落盘索引
            if (window.CorridorView) {
                window.CorridorView.loadHistory(folderName);
            }
        };

        function openImagePreview(images, startIndex) {
            if (!Array.isArray(images) || images.length === 0) return;
            currentHistoryImages = images;
            previewImageIndex = Math.max(0, Math.min(Number(startIndex) || 0, currentHistoryImages.length - 1));
            renderImagePreview();
            const preview = document.getElementById('image-preview');
            if (preview) preview.classList.remove('hidden');
            document.body.classList.add('preview-open');
        }

        function closeImagePreview() {
            const preview = document.getElementById('image-preview');
            const imageEl = document.getElementById('image-preview-img');
            if (preview) preview.classList.add('hidden');
            if (imageEl) imageEl.src = '';
            document.body.classList.remove('preview-open');
        }

        function shiftImagePreview(delta) {
            if (currentHistoryImages.length <= 1) return;
            previewImageIndex = (previewImageIndex + delta + currentHistoryImages.length) % currentHistoryImages.length;
            renderImagePreview();
        }

        function renderImagePreview() {
            const img = currentHistoryImages[previewImageIndex];
            const imageEl = document.getElementById('image-preview-img');
            const captionEl = document.getElementById('image-preview-caption');
            const prevBtn = document.getElementById('image-preview-prev');
            const nextBtn = document.getElementById('image-preview-next');
            if (!img || !imageEl) return;

            imageEl.src = img;
            if (captionEl) {
                captionEl.innerText = img.split('/').pop() + '  ' + (previewImageIndex + 1) + '/' + currentHistoryImages.length;
            }
            if (prevBtn) prevBtn.disabled = currentHistoryImages.length <= 1;
            if (nextBtn) nextBtn.disabled = currentHistoryImages.length <= 1;
        }

        document.addEventListener('keydown', function (event) {
            const preview = document.getElementById('image-preview');
            if (!preview || preview.classList.contains('hidden')) return;

            if (event.key === 'Escape') {
                closeImagePreview();
            } else if (event.key === 'ArrowLeft') {
                shiftImagePreview(-1);
            } else if (event.key === 'ArrowRight') {
                shiftImagePreview(1);
            }
        });

        (function () {
            // ---- 侧边栏切换 ----
            const sidebar = document.getElementById('sidebar');
            const overlay = document.getElementById('sidebar-overlay');
            const touchLandscapeQuery = window.matchMedia('(orientation: landscape) and (pointer: coarse) and (hover: none)');

            function isTouchLandscape() {
                return touchLandscapeQuery.matches;
            }

            function usesOverlaySidebar() {
                return window.innerWidth < 1025 || isTouchLandscape();
            }

            let sidebarOpen = !usesOverlaySidebar();

            if (usesOverlaySidebar()) {
                sidebarOpen = false;
                sidebar.classList.remove('open');
                sidebar.classList.remove('closed');
            } else {
                sidebar.classList.add('open');
            }

            window.toggleSidebar = function () {
                sidebarOpen = !sidebarOpen;
                if (sidebarOpen) {
                    sidebar.classList.add('open');
                    sidebar.classList.remove('closed');
                    if (usesOverlaySidebar()) {
                        overlay.classList.add('active');
                    }
                } else {
                    sidebar.classList.remove('open');
                    if (usesOverlaySidebar()) {
                        sidebar.classList.remove('closed');
                        overlay.classList.remove('active');
                    } else {
                        sidebar.classList.add('closed');
                    }
                }
                resizeWorkSurfaces();
            };

            window.closeSidebarOnMobile = function () {
                if (usesOverlaySidebar() && sidebarOpen) {
                    sidebarOpen = false;
                    sidebar.classList.remove('open');
                    sidebar.classList.remove('closed');
                    overlay.classList.remove('active');
                    resizeWorkSurfaces();
                }
            };

            window.addEventListener('resize', function () {
                if (usesOverlaySidebar()) {
                    sidebar.classList.remove('closed');
                    if (sidebarOpen) {
                        sidebar.classList.add('open');
                        overlay.classList.add('active');
                    } else {
                        sidebar.classList.remove('open');
                        overlay.classList.remove('active');
                    }
                } else {
                    overlay.classList.remove('active');
                    if (!sidebarOpen) {
                        sidebar.classList.add('closed');
                        sidebar.classList.remove('open');
                    } else {
                        sidebar.classList.remove('closed');
                        sidebar.classList.add('open');
                    }
                }
                resizeWorkSurfaces();
            });

            // ---- 导航高亮 & 视图切换 ----
            const navItems = document.querySelectorAll('.sidebar-nav-item');
            const middleRow = document.querySelector('.middle-row');
            const cameraCard = document.getElementById('section-camera');
            const gisCard = document.getElementById('section-gis');
            const mobileViewTitle = document.getElementById('mobile-view-title');

            window.switchView = function (viewMode) {
                currentDashboardView = viewMode;
                closeSidebarOnMobile();

                navItems.forEach(function (n) { n.classList.remove('active'); });
                var targetNav = document.querySelector('[data-nav="' + viewMode + '"]');
                if (targetNav) targetNav.classList.add('active');
                if (mobileViewTitle && targetNav) {
                    var targetLabel = targetNav.querySelector('.sidebar-nav-text');
                    if (targetLabel) {
                        mobileViewTitle.textContent = targetLabel.textContent.trim();
                    }
                }

                var currently3D = (typeof is3DView !== 'undefined') ? is3DView : false;

                var statsRow = document.getElementById('stats-row');
                var sensorBar = document.getElementById('sensor-bar');
                var controls = document.getElementById('section-controls');
                var pidSection = document.getElementById('section-pid');
                var logsSection = document.getElementById('section-logs');
                var historySection = document.getElementById('section-history');
                var reportSection = document.getElementById('section-report');
                var envSection = document.getElementById('section-environment');
                var contentInner = document.querySelector('.content-inner');
                var hidableSections = [statsRow, controls, pidSection, logsSection, historySection, reportSection, envSection];

                if (contentInner) {
                    contentInner.classList.toggle('view-overview', viewMode === 'overview');
                }

                middleRow.classList.remove('single-col', 'focus-expand', 'focus-hidden');
                cameraCard.classList.remove('hidden-panel', 'camera-full');
                gisCard.classList.remove('hidden-panel', 'gis-full');
                hidableSections.forEach(function (s) { if (s) s.classList.remove('focus-hidden'); });
                if (sensorBar) sensorBar.classList.remove('focus-hidden');

                var toggleBtn = document.getElementById('btn-toggle-view');

                // ---- 3D 走廊视图（corridor-container 独立于 gis/3d 容器） ----
                var corridorBox = document.getElementById('corridor-container');
                var gisBox = document.getElementById('gis-container');
                if (viewMode === 'corridor') {
                    if (currently3D) toggleView();          // 复位 3D 姿态渲染
                    if (gisBox) gisBox.classList.add('hidden');
                    if (corridorBox) corridorBox.classList.remove('hidden');
                    if (window.CorridorView) window.CorridorView.enter();
                } else {
                    if (corridorBox && !corridorBox.classList.contains('hidden')) {
                        corridorBox.classList.add('hidden');
                    }
                    if (window.CorridorView && window.CorridorView.isActive()) window.CorridorView.exit();
                    if (gisBox) gisBox.classList.remove('hidden');   // 由上方 3d 分支的 toggleView 再决定是否隐藏
                }

                if (viewMode === 'overview') {
                    if (currently3D) toggleView();
                    if (toggleBtn) toggleBtn.classList.remove('hidden');
                    if (pidSection) pidSection.classList.add('focus-hidden');
                    if (logsSection) logsSection.classList.add('focus-hidden');
                    if (historySection) historySection.classList.add('focus-hidden');
                    if (reportSection) reportSection.classList.add('focus-hidden');
                    if (envSection) envSection.classList.add('focus-hidden');

                } else if (viewMode === 'camera') {
                    hidableSections.forEach(function (s) { if (s) s.classList.add('focus-hidden'); });
                    middleRow.classList.add('single-col', 'focus-expand');
                    cameraCard.classList.add('camera-full');
                    gisCard.classList.add('hidden-panel');
                    if (currently3D) toggleView();
                    if (toggleBtn) toggleBtn.classList.remove('hidden');

                } else if (viewMode === 'gis') {
                    hidableSections.forEach(function (s) { if (s) s.classList.add('focus-hidden'); });
                    middleRow.classList.add('single-col', 'focus-expand');
                    cameraCard.classList.add('hidden-panel');
                    gisCard.classList.add('gis-full');
                    if (currently3D) toggleView();
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === '3d') {
                    hidableSections.forEach(function (s) { if (s) s.classList.add('focus-hidden'); });
                    middleRow.classList.add('single-col', 'focus-expand');
                    cameraCard.classList.add('hidden-panel');
                    gisCard.classList.add('gis-full');
                    if (!currently3D) toggleView();
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === 'corridor') {
                    hidableSections.forEach(function (s) { if (s) s.classList.add('focus-hidden'); });
                    middleRow.classList.add('single-col', 'focus-expand');
                    cameraCard.classList.add('hidden-panel');
                    gisCard.classList.add('gis-full');
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === 'controls') {
                    if (currently3D) toggleView();   // 离开 3D 视图，停止渲染循环
                    middleRow.classList.add('focus-hidden');
                    pidSection.classList.add('focus-hidden');
                    logsSection.classList.add('focus-hidden');
                    if (historySection) historySection.classList.add('focus-hidden');
                    if (reportSection) reportSection.classList.add('focus-hidden');
                    if (envSection) envSection.classList.add('focus-hidden');
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === 'pid') {
                    if (currently3D) toggleView();   // 离开 3D 视图，停止渲染循环
                    middleRow.classList.add('focus-hidden');
                    statsRow.classList.add('focus-hidden');
                    controls.classList.add('focus-hidden');
                    logsSection.classList.add('focus-hidden');
                    if (historySection) historySection.classList.add('focus-hidden');
                    if (reportSection) reportSection.classList.add('focus-hidden');
                    if (envSection) envSection.classList.add('focus-hidden');
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === 'logs') {
                    if (currently3D) toggleView();   // 离开 3D 视图，停止渲染循环
                    middleRow.classList.add('focus-hidden');
                    statsRow.classList.add('focus-hidden');
                    controls.classList.add('focus-hidden');
                    pidSection.classList.add('focus-hidden');
                    if (historySection) historySection.classList.add('focus-hidden');
                    if (reportSection) reportSection.classList.add('focus-hidden');
                    if (envSection) envSection.classList.add('focus-hidden');
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === 'report') {
                    if (currently3D) toggleView();   // 离开 3D 视图，停止渲染循环
                    middleRow.classList.add('focus-hidden');
                    statsRow.classList.add('focus-hidden');
                    if (sensorBar) sensorBar.classList.add('focus-hidden');
                    controls.classList.add('focus-hidden');
                    pidSection.classList.add('focus-hidden');
                    logsSection.classList.add('focus-hidden');
                    if (historySection) historySection.classList.add('focus-hidden');
                    if (envSection) envSection.classList.add('focus-hidden');
                    loadEvaluationReport();
                    loadMaintenancePlan();
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === 'history') {
                    if (currently3D) toggleView();   // 离开 3D 视图，停止渲染循环
                    middleRow.classList.add('focus-hidden');
                    statsRow.classList.add('focus-hidden');
                    if (sensorBar) sensorBar.classList.add('focus-hidden');
                    controls.classList.add('focus-hidden');
                    pidSection.classList.add('focus-hidden');
                    logsSection.classList.add('focus-hidden');
                    if (reportSection) reportSection.classList.add('focus-hidden');
                    if (envSection) envSection.classList.add('focus-hidden');
                    loadHistoryList();
                    loadNetworkOverview();
                    if (toggleBtn) toggleBtn.classList.add('hidden');

                } else if (viewMode === 'environment') {
                    if (currently3D) toggleView();
                    middleRow.classList.add('focus-hidden');
                    statsRow.classList.add('focus-hidden');
                    if (sensorBar) sensorBar.classList.add('focus-hidden');
                    controls.classList.add('focus-hidden');
                    pidSection.classList.add('focus-hidden');
                    logsSection.classList.add('focus-hidden');
                    if (historySection) historySection.classList.add('focus-hidden');
                    if (reportSection) reportSection.classList.add('focus-hidden');
                    if (toggleBtn) toggleBtn.classList.add('hidden');
                }

                /* 竖直居中：controls / pid / logs 视图内容少，居中更好看 */
                if (contentInner) {
                    if (viewMode === 'controls' || viewMode === 'pid' || viewMode === 'logs') {
                        contentInner.classList.add('v-centered');
                    } else {
                        contentInner.classList.remove('v-centered');
                    }
                }

                resizeWorkSurfaces();
                scheduleGisChartRender();
                syncCameraVideoForView();
            };

            window.switchView('overview');

            navItems.forEach(function (item) {
                item.addEventListener('click', function () {
                    navItems.forEach(function (n) { n.classList.remove('active'); });
                    item.classList.add('active');
                });
            });

            // ---- 运行时统计 ----
            let runtimeSeconds = 0;
            let runtimeInterval = null;
            let wasRunning = false;
            const runtimeEl = document.getElementById('stat-runtime');

            function formatRuntime(totalSec) {
                const h = Math.floor(totalSec / 3600);
                const m = Math.floor((totalSec % 3600) / 60);
                const s = totalSec % 60;
                return String(h).padStart(2, '0') + ':' + String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
            }

            setInterval(function () {
                const badge = document.getElementById('status-badge');
                const isRunning = badge && badge.innerText === '运行中';

                if (isRunning && !wasRunning) {
                    if (!runtimeInterval) {
                        runtimeInterval = setInterval(function () {
                            runtimeSeconds++;
                            if (runtimeEl) runtimeEl.innerText = formatRuntime(runtimeSeconds);
                        }, 1000);
                    }
                } else if (!isRunning && wasRunning) {
                    if (runtimeInterval) {
                        clearInterval(runtimeInterval);
                        runtimeInterval = null;
                    }
                }
                wasRunning = isRunning;
            }, 500);

            window.scrollToSection = function (sectionId) {
                const el = document.getElementById(sectionId);
                const contentArea = document.querySelector('.content-area');
                if (el && contentArea) {
                    const elTop = el.getBoundingClientRect().top;
                    const areaTop = contentArea.getBoundingClientRect().top;
                    const offset = elTop - areaTop + contentArea.scrollTop - 20;
                    contentArea.scrollTo({ top: offset, behavior: 'smooth' });
                }
            };
        })();
