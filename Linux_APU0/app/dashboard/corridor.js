/*
 * 3D 管道走廊渲染器 v8（2026-10-27）
 * ====================
 * v8（用户拍板：按实物 L 形演示管锁死几何 + 缺陷照片按日志对齐贴壁）：
 *   - 后端 corridor.py 把里程投影到锁死轨道（0.9m 直道 + 左转 90°/R0.30m
 *     + 转后直道，总长 1.8m），断面/机器人位姿不再信遥测 yaw
 *     —— 根治"没转弯却画出 1.8m 大直道（直道 1.5m 标尺）"
 *   - 缺陷日志的 5 张实拍图按实拍画面定墙、按日志真实里程贴壁（实物缺陷
 *     全在入口前 60cm）：左壁 裂缝/破洞，右壁 泥沙淤积/管壁脱落，
 *     顶部 树根侵入；同侧过近由后端排开（最小 0.18m），照片随管壁生长到位
 *     才出现；姿态用显式基向量与管壁相切（不再 lookAt 歪斜），
 *     不再用检测框针孔投影（贴片会飘出管外）
 * ====================
 * 数据源：/api/corridor/current（1s 轮询）
 *   - sections   断面（几何，按里程步长 0.25m + 弯道自适应加密）
 *   - keyframes  视觉关键帧（后端已做相似度去重，只走右侧贴牌展示）
 * 渲染（three.js，全部跑在浏览器 GPU，板卡零渲染负担）：
 *   - 连续管壁：一条共享断面环顶点的 tube 随巡检生长，几何上无段间缝隙
 *   - 管内紧贴跟车视角（1.0 倍管径）：车尾后座看管道纵深
 *   - 水下氛围：青绿色调 + 管内悬浮颗粒 + 收敛版车灯体积光锥 + 雾随管径缩放
 *   - 每 0.5m 里程标尺环（带数字牌）+ 入口法兰；拖拽环绕/滚轮缩放/双击复位
 *   - 缺陷实拍照片贴墙（有缺陷才贴、没有不贴；无实拍时回退等级文字钉）
 *   - 关键帧只由后端存档进巡检目录，不再上墙（2026-10-18 用户拍板）
 *
 * v6 修复（用户截图反馈：机器人轮廓混乱 + 管外玻璃泡/巨球/乱点云）：
 *   - robot.glb 实测为 Y-up/Z-前水平建模（包围盒 x±19/y±9.5/z-23~+19），
 *     之前的 rotation.x=-π/2 "趴平"反而把它竖立 → 删常量旋转，模型回正；
 *     glb 加载成功后撤掉兜底小车（此前两个模型重叠渲染）
 *   - 管壁圆柱段+弯道补球方案 → 连续 tube（补球 BackSide 从管外看=一串玻璃泡）
 *   - 悬浮颗粒收敛到管内（±0.25m/±0.22m/车后0.3m~车前1.6m）
 *   - 车灯体积光锥收敛（2.6d→1.6d 长，透明度 0.10→0.07）
 *
 * 暴露 window.CorridorView = { enter, exit, isActive, loadHistory }
 * 由 app.js 的 switchView('corridor') 调用，惰性初始化。
 */
(function () {
    'use strict';
    if (typeof THREE === 'undefined') { return; }

    var POLL_MS = 1000;
    var DEG = Math.PI / 180;

    var container = null;
    var renderer = null, scene = null, camera = null;
    var group = null;               // 走廊所有静态子对象父组
    var robotCarrier = null;        // 机器人载体（含 glb 或 fallback）
    var robotModel = null;
    var spotLight = null, spotTarget = null, glowLight = null;
    var hudEls = {};
    var resizeObserver = null;

    var active = false;
    var rafId = null, pollTimer = null;
    var lastRenderTs = 0;
    var replayMode = false;        // 历史回放模式：停用实时轮询
    var replayFolder = '';
    var needSnap = true;           // 位姿吸附标志：首次拿到数据/换新巡检时，
                                   // 机器人直接就位，不从原点"飞"过来（2026-10-17）

    var sections = [];
    var keyframes = [];
    var tubeMesh = null;            // 连续管壁（一条随断面生长的 tube，v6）
    var tubeBuilt = 0;              // 已纳入 tube 的断面数
    var builtRings = 0;             // 已建接缝环数（每断面一个）
    var lastRulerK = -1;            // 最近一个里程标尺环的档位（0.5m 一档）

    // ---- 水下氛围（2026-10-17）：悬浮颗粒 + 车灯体积光锥 ----
    var particleGroup = null, particlePts = null, particleSpeeds = null;
    var PARTICLE_N = 260;
    var lightCone = null;
    var FORWARD_Z = new THREE.Vector3(0, 0, 1);
    var CONE_AXIS = new THREE.Vector3(0, -1, 0);   // 光锥几何 apex 朝 -Y
    var builtDefects = {};
    var defectSprites = [];
    var latestData = null;
    // 本页面会话里是否见过"巡检进行中"：里程只在这次巡检结束后才报数，
    // 刚打开页面/待巡检状态一律显示 ——（2026-10-27 用户拍板）
    var sawRunning = false;

    // （2026-10-17 v5 已删：实时画面贴壁 segMeshes/segTexState/live 全套 ——
    //  车头两段管壁贴 /video_feed 与两侧程序化管壁质感完全不同，
    //  跑起来像一节"怪管子"跟着车头走，是"一截一截断面"感的来源之一；
    //  管壁统一程序化纹理，实时画面看"相机"视图即可）
    var texLoader = new THREE.TextureLoader();
    texLoader.crossOrigin = 'anonymous';

    // 跟车相机：az=绕车尾方位角(0=正后方)，el=抬升角(rad)，dist=管径倍数
    // ⚠️ el×dist×管径 必须小于管半径，否则相机穿出管顶 → 变成"管外斜看"
    // ⚠️ dist 必须够小：1.8m 演示管里 dist=1.8d≈1.44m 会让相机 80% 行程都
    //    留在管口外（用户反馈"不是内视角"的根因）→ v5 收紧到 1.0d（≈0.8m），
    //    紧贴车尾的管内跟随视角（对齐参考图：坐机器人后座上看管道深处）
    var FOLLOW_DEFAULT = { az: 0, el: 0.08, dist: 1.0 };
    var follow = { az: FOLLOW_DEFAULT.az, el: FOLLOW_DEFAULT.el, dist: FOLLOW_DEFAULT.dist };
    var robotPose = { x: 0, y: 0, z: 0, yaw: 0, pitch: 0 };
    var smoothPos = new THREE.Vector3();
    var smoothHeading = 0;
    var curDiameter = 0.8;          // 平滑后的当前管径（比例基准）

    var SEV_COLORS = { 1: '#fac775', 2: '#ef9f27', 3: '#e24b4a', 4: '#a32d2d' };

    // 后端坐标 (x东, y北, z高程) -> three (X=x, Y=z, Z=-y)
    function toVec(p) { return new THREE.Vector3(p.x, p.z, -p.y); }

    // ------------------------------------------------------------------
    // 初始化（惰性，首次 enter 时执行）
    // ------------------------------------------------------------------
    function init() {
        container = document.getElementById('corridor-container');
        if (!container) { return false; }

        scene = new THREE.Scene();
        // 水下青绿色调（2026-10-17：黑底灰管"塑料感"→水下管道氛围）
        scene.background = new THREE.Color(0x06121c);
        scene.fog = new THREE.Fog(0x06121c, 1.6, 9.6);   // 初始按 0.8m 管径，animate 里随实测管径缩放

        camera = new THREE.PerspectiveCamera(62, 1, 0.05, 120);

        renderer = new THREE.WebGLRenderer({ antialias: true });
        renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
        container.appendChild(renderer.domElement);
        if ('outputEncoding' in renderer && THREE.sRGBEncoding !== undefined) {
            renderer.outputEncoding = THREE.sRGBEncoding;
        }

        group = new THREE.Group();
        scene.add(group);

        // 灯光：暗环境 + 机器人头部车灯（SpotLight 主角）+ 头部近距泛光
        // （2026-10-18 逼真化：参考 10-01 黑暗隧道——环境光压到 0.30，
        //  只有车灯光池照亮前方一小块，其余隐入黑暗；车灯束收窄更聚拢）
        scene.add(new THREE.AmbientLight(0x86b4bc, 0.30));
        glowLight = new THREE.PointLight(0xaec3e8, 1.1, 4.0);
        scene.add(glowLight);
        spotLight = new THREE.SpotLight(0xffd9a0, 6.5, 18, 0.55, 0.70, 0.9);
        spotTarget = new THREE.Object3D();
        scene.add(spotTarget);
        spotLight.target = spotTarget;
        scene.add(spotLight);

        buildRobot();
        buildHud();
        bindControls();
        initParticles();    // 水下悬浮颗粒
        initLightCone();    // 车灯体积光锥

        resizeObserver = new ResizeObserver(function () { resize(); });
        resizeObserver.observe(container);
        return true;
    }

    function buildRobot() {
        robotCarrier = new THREE.Group();

        // fallback：简易小车（glb 加载失败时使用；本体长 ~0.58m）
        var fallbackGroup = new THREE.Group();
        var body = new THREE.Mesh(
            new THREE.BoxGeometry(0.30, 0.16, 0.44),
            new THREE.MeshStandardMaterial({ color: 0x9fb4cc, roughness: 0.6, metalness: 0.3 })
        );
        body.position.y = 0.11;
        var cam = new THREE.Mesh(
            new THREE.BoxGeometry(0.11, 0.08, 0.10),
            new THREE.MeshStandardMaterial({ color: 0x2b3a52, roughness: 0.4 })
        );
        cam.position.set(0, 0.21, 0.17);
        var lampMat = new THREE.MeshStandardMaterial({
            color: 0x332200, emissive: 0xffdf9e, emissiveIntensity: 2.2
        });
        var lampL = new THREE.Mesh(new THREE.SphereGeometry(0.035, 10, 8), lampMat);
        lampL.position.set(-0.09, 0.16, 0.24);
        var lampR = lampL.clone();
        lampR.position.x = 0.09;
        fallbackGroup.add(body, cam, lampL, lampR);
        robotCarrier.add(fallbackGroup);

        if (typeof THREE.GLTFLoader === 'function') {
            new THREE.GLTFLoader().load('/static/models/robot.glb', function (gltf) {
                robotModel = gltf.scene;
                robotModel.scale.set(0.04, 0.04, 0.04);
                // 2026-10-18 修正：glb 节点自带 +90°X（Blender Z-up 导出转换），
                // 舱体轴竖直 → 再 -90°X 放平、舱轴朝 +Z。该旋转是【朝向校正】
                // 常量，只挂内层，与载体（carrier）的航向/俯仰动画互不干扰。
                robotModel.rotation.x = -Math.PI / 2;
                var inner = new THREE.Group();
                inner.add(robotModel);
                inner.rotation.y = Math.PI;   // 车头对准走廊前进方向
                robotCarrier.add(inner);
                robotCarrier.remove(fallbackGroup);   // glb 就位，撤掉兜底小车
            }, undefined, function () { /* 保留 fallback */ });
        }
        scene.add(robotCarrier);
    }

    function buildHud() {
        function el(cls, html) {
            var d = document.createElement('div');
            d.className = cls;
            d.innerHTML = html;
            d.style.cssText += ';pointer-events:none;position:absolute;z-index:10;';
            container.appendChild(d);
            return d;
        }
        hudEls.mode = el('corridor-hud-mode',
            '<span style="font-size:13px;font-weight:600;color:#dbe6f5;">3D 管道走廊</span>' +
            '&nbsp;<span id="corridor-mode-tag" style="font-size:11px;padding:1px 8px;border-radius:9px;background:#243247;color:#8fb7e8;">待巡检</span>');
        hudEls.mode.style.cssText += ';left:14px;top:12px;';

        hudEls.stats = el('corridor-hud-stats', '');
        hudEls.stats.style.cssText += ';right:14px;top:12px;text-align:right;';

        hudEls.hint = el('corridor-hud-hint',
            '<span style="font-size:11px;color:#7286a3;">拖拽环绕 · 滚轮缩放 · 双击复位跟车视角</span>');
        hudEls.hint.style.cssText += ';left:50%;bottom:10px;transform:translateX(-50%);';
    }

    function updateHud() {
        if (!latestData) { return; }
        var d = latestData;
        var tag = hudEls.mode && hudEls.mode.querySelector('#corridor-mode-tag');
        if (tag) {
            if (replayMode) {
                tag.textContent = '回放 · ' + replayFolder.replace(/^Inspect_/, '');
                tag.style.background = '#33254d'; tag.style.color = '#c9a8f0';
            } else if (!d.folder) {
                // 本次巡检已收尾（本会话里跑过、断面已生成）→ 已完成，才报里程
                if (sawRunning && d.sections && d.sections.length > 1) {
                    tag.textContent = '已完成';
                    tag.style.background = '#1f3a2c'; tag.style.color = '#8fe0ae';
                } else {
                    tag.textContent = '待巡检';
                    tag.style.background = '#243247'; tag.style.color = '#8fb7e8';
                }
            } else if (d.demo_mode) {
                tag.textContent = '演示模式'; tag.style.background = '#3d3323'; tag.style.color = '#e8c88f';
            } else {
                tag.textContent = '实况重建'; tag.style.background = '#1f3a2c'; tag.style.color = '#8fe0ae';
            }
        }
        var vis = 0;
        (d.defects || []).forEach(function (x) { if (x.visible !== false) { vis++; } });
        // 里程只在本次巡检结束后才显示：巡检中/待巡检/刚打开页面一律 "——"
        var showMileage = replayMode ||
            (!d.running && sawRunning && d.sections && d.sections.length > 1);
        var mileHtml = showMileage
            ? '<b style="color:#e8eef7;">' + d.total_mileage.toFixed(1) + '</b> m'
            : '<b style="color:#5d6b80;">——</b>';
        if (hudEls.stats) {
            hudEls.stats.innerHTML =
                '<div style="font-size:12px;color:#9fb0c8;">里程 ' + mileHtml + '</div>' +
                '<div style="font-size:12px;color:#9fb0c8;">断面 <b style="color:#e8eef7;">' +
                (d.sections ? d.sections.length : 0) + '</b> · 关键帧 <b style="color:#e8eef7;">' +
                keyframes.length + '</b> · 缺陷 <b style="color:#ff9b9b;">' + vis + '</b></div>';
        }
    }

    // ------------------------------------------------------------------
    // 程序化管壁纹理（2026-10-18 v7 逼真版：参考 10-01 黑暗隧道效果）
    // ------------------------------------------------------------------
    // 用户拍板：实拍帧可能模糊，不贴照片，用程序化假管但要逼真。
    // 逼真感的灵魂是"暗"——很暗的混凝土基底 + 环境光压低，只有车灯光池
    // 照亮前方一小块，其余隐入黑暗（见 init 灯光与 animate 雾距）。
    // 纹理细节：每米一道环向接缝（真管接口）、纵向水渍、管底积垢带、噪点。
    var wallTex = null;

    function buildWallTexture() {
        var c = document.createElement('canvas');
        c.width = 512; c.height = 512;   // u=绕管一圈，v=1m 重复
        var g = c.getContext('2d');
        // 基底：很暗的蓝灰混凝土（横向微渐变，管侧略亮、管顶底略暗）
        var grad = g.createLinearGradient(0, 0, 512, 0);
        grad.addColorStop(0, '#232a34'); grad.addColorStop(0.25, '#2a323d');
        grad.addColorStop(0.5, '#232a34'); grad.addColorStop(0.75, '#2a323d');
        grad.addColorStop(1, '#232a34');
        g.fillStyle = grad; g.fillRect(0, 0, 512, 512);
        // 管底积垢带（u≈0.65~0.9 对应管底，更深更脏）
        var grime = g.createLinearGradient(330, 0, 460, 0);
        grime.addColorStop(0, 'rgba(10,13,18,0)');
        grime.addColorStop(0.5, 'rgba(10,13,18,0.42)');
        grime.addColorStop(1, 'rgba(10,13,18,0)');
        g.fillStyle = grime; g.fillRect(330, 0, 130, 512);
        // 低频污渍斑
        for (var i = 0; i < 36; i++) {
            var x = Math.random() * 512, y = Math.random() * 512;
            var rr = 16 + Math.random() * 64;
            var rg = g.createRadialGradient(x, y, 2, x, y, rr);
            rg.addColorStop(0, Math.random() < 0.6 ?
                'rgba(12,16,23,0.20)' : 'rgba(120,132,150,0.06)');
            rg.addColorStop(1, 'rgba(0,0,0,0)');
            g.fillStyle = rg;
            g.beginPath(); g.arc(x, y, rr, 0, 6.3); g.fill();
        }
        // 纵向水渍流痕（沿管长方向，细而淡）
        for (var ws = 0; ws < 26; ws++) {
            var wx = Math.random() * 512;
            var wy = Math.random() * 512;
            var wlen = 40 + Math.random() * 140;
            var wg = g.createLinearGradient(0, wy, 0, wy + wlen);
            wg.addColorStop(0, 'rgba(14,19,26,0.28)');
            wg.addColorStop(1, 'rgba(14,19,26,0)');
            g.fillStyle = wg;
            g.fillRect(wx, wy, 1.5 + Math.random() * 3, wlen);
        }
        // 骨料噪点
        for (var j = 0; j < 3200; j++) {
            g.fillStyle = Math.random() < 0.5 ?
                'rgba(10,14,20,0.18)' : 'rgba(140,152,170,0.06)';
            g.fillRect(Math.random() * 512, Math.random() * 512, 1.5, 1.5);
        }
        // 环向接缝：每米一道主缝（v 方向 1 次重复 = 1m 一节管）
        g.strokeStyle = 'rgba(8,11,16,0.65)'; g.lineWidth = 4;
        g.beginPath(); g.moveTo(0, 8); g.lineTo(512, 8); g.stroke();
        g.strokeStyle = 'rgba(150,162,178,0.10)'; g.lineWidth = 1.5;
        g.beginPath(); g.moveTo(0, 13); g.lineTo(512, 13); g.stroke();  // 接缝高光边

        wallTex = new THREE.CanvasTexture(c);
        wallTex.wrapS = THREE.RepeatWrapping;
        wallTex.wrapT = THREE.RepeatWrapping;
        if (THREE.sRGBEncoding !== undefined && 'encoding' in wallTex) {
            wallTex.encoding = THREE.sRGBEncoding;
        }
    }

    // ------------------------------------------------------------------
    // 水下氛围：悬浮颗粒（dust motes）+ 车灯体积光锥
    // ------------------------------------------------------------------
    function initParticles() {
        // 局部坐标系跟随机器人：+Z 朝车头前方。
        // 2026-10-17 收敛到管内（之前 ±0.8m 横向/车前 4m，颗粒漂到管壁外
        // 和管道尽头外的虚空里，管外视角像一团乱点云）
        var pos = new Float32Array(PARTICLE_N * 3);
        particleSpeeds = new Float32Array(PARTICLE_N);
        for (var i = 0; i < PARTICLE_N; i++) {
            pos[i * 3] = (Math.random() - 0.5) * 0.5;        // 横向 ±0.25m
            pos[i * 3 + 1] = (Math.random() - 0.5) * 0.44;   // 垂向 ±0.22m
            pos[i * 3 + 2] = -0.3 + Math.random() * 1.9;     // 车后 0.3m ~ 车前 1.6m
            particleSpeeds[i] = 0.5 + Math.random();
        }
        var geo = new THREE.BufferGeometry();
        geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
        particlePts = new THREE.Points(geo, new THREE.PointsMaterial({
            color: 0xa8d8d4, size: 0.010, transparent: true, opacity: 0.45,
            blending: THREE.AdditiveBlending, depthWrite: false,
            sizeAttenuation: true
        }));
        particleGroup = new THREE.Group();
        particleGroup.add(particlePts);
        scene.add(particleGroup);
    }

    function initLightCone() {
        // 单位锥（半径1/高1，apex 平移到原点、向 -Y 展开），animate 里按管径缩放
        var geo = new THREE.ConeGeometry(1, 1, 24, 1, true);
        geo.translate(0, -0.5, 0);
        lightCone = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({
            color: 0xffdfae, transparent: true, opacity: 0.07,
            blending: THREE.AdditiveBlending, depthWrite: false,
            side: THREE.DoubleSide, fog: false
        }));
        lightCone.renderOrder = 5;
        scene.add(lightCone);
    }

    // ------------------------------------------------------------------
    // 走廊构建（增量）
    // ------------------------------------------------------------------
    function headingRadOf(sec) {
        // 后端平面航向（rad），0 = +x（东）
        return (sec.yaw || 0) * DEG;
    }

    // 锁死 L 形轨道（v8）：里程 -> { pos(three), h(rad) }。
    // 参数由后端 /api/corridor/current 的 track 字段下发，与 corridor.py 同公式。
    function trackPose(m, tr) {
        var s1 = tr.straight1_m, R = tr.turn_radius_m, deg = tr.turn_deg;
        var arc = R * deg * DEG;
        m = Math.max(0, Math.min(m, tr.total_m));
        var bx, by, yawDeg;              // 后端平面系 (x=入口方向, y=左转方向)
        if (m <= s1) {
            bx = m; by = 0; yawDeg = 0;
        } else if (m <= s1 + arc) {
            var th = (m - s1) / R;
            bx = s1 + R * Math.sin(th);
            by = R * (1 - Math.cos(th));
            yawDeg = th / DEG;
        } else {
            bx = s1 + R;
            by = R + (m - s1 - arc);
            yawDeg = deg;
        }
        return { pos: new THREE.Vector3(bx, 0, -by), h: yawDeg * DEG };
    }

    function smoothedRadius(i) {
        // 管径平滑（2026-10-17）：±1 断面滑动平均。
        // 超声实测管径逐断面抖动会让管壁变成"罗纹管"，平滑后才顺。
        // 2026-10-18 加显示侧兜底钳制 [0.12, 0.60]m（管径 0.24~1.20m），
        // 陆上调试超声乱反射时也不会出现比管还大的环。
        var sum = 0, n = 0;
        for (var j = i - 1; j <= i + 1; j++) {
            if (j >= 0 && j < sections.length) {
                sum += (sections[j].diameter || 0.8); n++;
            }
        }
        if (!n) { return 0.4; }
        var r = (sum / n) / 2;
        return Math.min(0.60, Math.max(0.12, r));
    }

    var RULER_STEP_M = 0.5;   // 里程标尺环间隔（1.8m 演示管：0.5m 一档）

    function rulerLabelTexture(text) {
        var c = document.createElement('canvas');
        c.width = 256; c.height = 96;
        var g = c.getContext('2d');
        g.fillStyle = 'rgba(8,16,26,0.72)';
        g.beginPath();
        if (g.roundRect) { g.roundRect(4, 10, 248, 76, 12); g.fill(); }
        else { g.fillRect(4, 10, 248, 76); }
        g.fillStyle = '#bfe3ea';
        g.font = '600 44px "Segoe UI", "Microsoft YaHei", sans-serif';
        g.textAlign = 'center';
        g.fillText(text, 128, 64);
        var tex = new THREE.CanvasTexture(c);
        if (THREE.sRGBEncoding !== undefined && 'encoding' in tex) {
            tex.encoding = THREE.sRGBEncoding;
        }
        return tex;
    }

    function buildRing(i) {
        // v6：普通断面什么都不画（管壁已是连续 tube，无缝可补）；
        //   仅入口（加粗法兰）+ 每 0.5m 标尺环画 torus
        var sec = sections[i];
        var r = smoothedRadius(i);
        var pos = toVec(sec);
        var h = headingRadOf(sec);
        var dir = new THREE.Vector3(Math.cos(h), 0, -Math.sin(h));
        var m = sec.mileage || 0;
        var rulerK = Math.floor(m / RULER_STEP_M + 1e-6);
        // rulerK>=1：起点段（0~0.5m）不出 "0.0 m" 标尺环
        var isRuler = (i > 0 && rulerK >= 1 && rulerK > lastRulerK);
        if (isRuler) { lastRulerK = rulerK; }
        var entrance = (i === 0);
        if (!entrance && !isRuler) { return; }   // 普通断面不画环
        // 2026-10-18：标尺环调暗调细（此前亮青色粗环在外视角像"分段箍"），
        // 入口法兰保留亮蓝作为起点标识
        var ring = new THREE.Mesh(
            new THREE.TorusGeometry(r * 0.99,
                entrance ? r * 0.06 : Math.max(r * 0.028, 0.006), 8, 40),
            new THREE.MeshStandardMaterial({
                color: entrance ? 0x4f8fbf : 0x2c4a60,
                roughness: 0.7, metalness: 0.3,
                emissive: entrance ? 0x2a5f8e : 0x0e2331,
                emissiveIntensity: entrance ? 1.0 : 0.4
            })
        );
        ring.position.copy(pos);
        ring.lookAt(pos.clone().add(dir));   // 环面法线 = 行进方向
        group.add(ring);
        if (isRuler) {
            // 里程数字牌：挂在标尺环上方管内（0.78r 保持在管壁内侧，否则被墙挡住）
            // 2026-10-18 缩小（r*0.55×r*0.21）：近距离跟车时不再整屏挡视野
            var sp = new THREE.Sprite(new THREE.SpriteMaterial({
                map: rulerLabelTexture((rulerK * RULER_STEP_M).toFixed(1) + ' m'),
                transparent: true, depthWrite: false, opacity: 0.85
            }));
            sp.position.copy(pos).add(new THREE.Vector3(0, r * 0.78, 0));
            sp.scale.set(r * 0.55, r * 0.21, 1);
            group.add(sp);
        }
    }

    // （2026-10-17 已删：findKfForSegment / photoWallMaterial / backfillPhotoTextures
    //  —— 前视相机帧几何上不属于该里程的侧壁，整管缠绕只会糊成色块；
    //  照片改走右侧管壁贴牌展示，管壁统一程序化混凝土纹理）

    // ---- 连续管壁（v6）：一条共享断面环顶点的 tube，无段间缝隙 ----
    // 旧方案=相邻圆管平口拼接：直道靠环形硬接、弯道有楔形缝，补球又从管外
    // 看成一串"玻璃泡"。现在每来一个断面重建一次整条 tube（断面数百个量级，
    // 浏览器每秒重建一次毫无压力），几何上天然连续。
    var TUBE_RADIAL = 28;

    function rebuildTube() {
        var n = sections.length;
        tubeBuilt = n;
        if (n < 2) { return; }
        if (!wallTex) { buildWallTexture(); }

        var cols = TUBE_RADIAL + 1;
        var pos = new Float32Array(n * cols * 3);
        var nor = new Float32Array(n * cols * 3);
        var uv = new Float32Array(n * cols * 2);
        for (var i = 0; i < n; i++) {
            var sec = sections[i];
            var c = toVec(sec);
            var h = headingRadOf(sec);
            var r = smoothedRadius(i);
            // 环平面基向量：right=水平垂直于行进方向，up=竖直（管道近水平）
            var rx = Math.cos(h + Math.PI / 2), rz = -Math.sin(h + Math.PI / 2);
            for (var j = 0; j <= TUBE_RADIAL; j++) {
                var th = (j / TUBE_RADIAL) * Math.PI * 2;
                var co = Math.cos(th), si = Math.sin(th);
                var vi = (i * cols + j) * 3;
                // 顶点 = 断面中心 + 半径×环向单位向量（法线朝外，BackSide
                // 材质在 shader 里自动翻转 → 管内光照正确）
                var nx = rx * co, ny = si, nz = rz * co;
                pos[vi] = c.x + nx * r;
                pos[vi + 1] = c.y + ny * r;
                pos[vi + 2] = c.z + nz * r;
                nor[vi] = nx; nor[vi + 1] = ny; nor[vi + 2] = nz;
                var ui = (i * cols + j) * 2;
                uv[ui] = j / TUBE_RADIAL;
                uv[ui + 1] = (sec.mileage || 0);   // v 按里程推进，纹理每 1m 重复（=每米一道接缝）
            }
        }
        var index = [];
        for (var a = 0; a < n - 1; a++) {
            for (var b = 0; b < TUBE_RADIAL; b++) {
                var r0 = a * cols + b;
                var r1 = r0 + cols;
                // 绕序保证几何正面朝管外（配合 BackSide 材质 → 管内可见）
                index.push(r0, r0 + 1, r1, r0 + 1, r1 + 1, r1);
            }
        }
        var geo = new THREE.BufferGeometry();
        geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
        geo.setAttribute('normal', new THREE.BufferAttribute(nor, 3));
        geo.setAttribute('uv', new THREE.BufferAttribute(uv, 2));
        geo.setIndex(index);
        if (tubeMesh) {
            tubeMesh.geometry.dispose();
            tubeMesh.geometry = geo;
        } else {
            // DoubleSide：管内看内壁、管外看连续实心管道（BackSide 从外看会
            // 透视到远侧内壁，产生"断层"感 —— 2026-10-18 用户反馈）
            tubeMesh = new THREE.Mesh(geo, new THREE.MeshStandardMaterial({
                map: wallTex, roughness: 0.95, metalness: 0.04,
                color: 0xffffff, side: THREE.DoubleSide
            }));
            group.add(tubeMesh);
        }
    }

    function nearestSection(mileage) {
        var idx = 0, best = Infinity;
        for (var i = 0; i < sections.length; i++) {
            var gap = Math.abs((sections[i].mileage || 0) - mileage);
            if (gap < best) { best = gap; idx = i; }
        }
        return sections[idx];
    }

    function defectSpriteTexture(d) {
        var c = document.createElement('canvas');
        c.width = 320; c.height = 140;
        var g = c.getContext('2d');
        var color = SEV_COLORS[d.severity_code] || SEV_COLORS[1];
        g.fillStyle = 'rgba(10,16,28,0.88)';
        g.beginPath();
        if (g.roundRect) { g.roundRect(6, 6, 308, 128, 14); g.fill(); }
        else { g.fillRect(6, 6, 308, 128); }
        g.strokeStyle = color; g.lineWidth = 4;
        g.beginPath();
        if (g.roundRect) { g.roundRect(6, 6, 308, 128, 14); g.stroke(); }
        else { g.strokeRect(6, 6, 308, 128); }
        g.fillStyle = color;
        g.fillRect(6, 6, 308, 10);
        g.fillStyle = '#f2f6fb';
        g.font = '600 40px "Segoe UI", "Microsoft YaHei", sans-serif';
        g.textAlign = 'center';
        g.fillText(d.type + ' ' + d.severity_code + '级', 160, 62);
        g.fillStyle = '#9fb0c8';
        g.font = '30px "Segoe UI", sans-serif';
        g.fillText('@ ' + d.mileage.toFixed(0) + ' m', 160, 108);
        return new THREE.CanvasTexture(c);
    }

    function defectPhotoTexture(d) {
        // 缺陷实拍照片牌：照片 + 等级色边框 + 底部信息条（类型/级别/里程）
        var c = document.createElement('canvas');
        c.width = 384; c.height = 288;
        var g = c.getContext('2d');
        var color = SEV_COLORS[d.severity_code] || SEV_COLORS[1];
        g.fillStyle = color; g.fillRect(0, 0, 384, 288);          // 等级色边框
        g.fillStyle = '#10161f'; g.fillRect(6, 6, 372, 276);      // 照片未加载先垫底
        var drawInfo = function () {
            g.fillStyle = 'rgba(8,13,22,0.85)';
            g.fillRect(6, 240, 372, 42);
            g.fillStyle = '#f2f6fb';
            g.font = '600 24px "Segoe UI", "Microsoft YaHei", sans-serif';
            g.textAlign = 'left';
            g.fillText(d.type + ' ' + d.severity_code + '级', 16, 270);
            g.fillStyle = '#9fb0c8';
            g.textAlign = 'right';
            g.fillText('@ ' + d.mileage.toFixed(1) + ' m', 368, 270);
        };
        drawInfo();
        var tex = new THREE.CanvasTexture(c);
        if (THREE.sRGBEncoding !== undefined && 'encoding' in tex) {
            tex.encoding = THREE.sRGBEncoding;
        }
        texLoader.load(d.image, function (t) {
            var img = t.image;
            var iw = img.width || 640, ih = img.height || 480;
            var s = Math.max(372 / iw, 234 / ih);
            var dw = iw * s, dh = ih * s;
            try {
                g.drawImage(img, (384 - dw) / 2, (246 - dh) / 2, dw, dh);
            } catch (e) { /* 跨域等异常保留底 */ }
            drawInfo();
            tex.needsUpdate = true;
        });
        return tex;
    }

    function defectPatchTexture(d) {
        // 从缺陷快照里裁出检测框区域当贴壁纹理（UV repeat/offset 裁剪），
        // 贴出来就是缺陷本身的管壁画面，而不是整张照片牌
        var bb = d.bbox || { x: 128, y: 140, w: 150, h: 120 };
        var m = 256, pad = 5;   // 外扩一点，避免把标注框线裁进纹理
        var x1 = Math.max(0, bb.x - bb.w / 2 - pad);
        var y1 = Math.max(0, bb.y - bb.h / 2 - pad);
        var x2 = Math.min(m, bb.x + bb.w / 2 + pad);
        var y2 = Math.min(m, bb.y + bb.h / 2 + pad);
        var tex = texLoader.load(d.image, function (t) { t.needsUpdate = true; });
        if (THREE.sRGBEncoding !== undefined && 'encoding' in tex) {
            tex.encoding = THREE.sRGBEncoding;
        }
        tex.wrapS = tex.wrapT = THREE.ClampToEdgeWrapping;
        tex.repeat.set((x2 - x1) / m, (y2 - y1) / m);
        tex.offset.set(x1 / m, 1 - y2 / m);   // UV 原点在左下
        return tex;
    }

    // v8 固定展位贴壁：缺陷日志的实拍图按实拍画面定墙、按日志真实里程定位置 ——
    //   左壁 裂缝/破洞，右壁 泥沙淤积/管壁脱落，顶部 树根侵入；
    //   里程直接用日志里程（实物缺陷都在入口前 60cm，后端已把同侧过近的排开）。
    // 贴片是**沿管壁弯曲的曲面**（12×3 细分，半径=管壁同一套平滑半径−4mm），
    // 不是平面卡片 —— 不会再把四角顶出管壁；UV 按管壁曲面展开，
    // 管内正视不歪、不镜像（顶部展位照片上沿朝行进方向）。
    function buildSlottedDefect(d, key) {
        var dm = (typeof d.display_mileage === 'number') ? d.display_mileage : d.mileage;
        // 管壁还没生长到展位里程：先不建，下轮轮询再试
        if (!replayMode && latestData &&
            (latestData.total_mileage || 0) < dm - 0.02) {
            delete builtDefects[key];
            return;
        }
        var tr = latestData && latestData.track;
        var basePos, h;
        if (tr && tr.fixed) {
            var tp = trackPose(dm, tr);
            basePos = tp.pos; h = tp.h;
        } else {
            var sec0 = nearestSection(dm);
            if (!sec0) { delete builtDefects[key]; return; }
            basePos = toVec(sec0); h = headingRadOf(sec0);
        }
        // 半径与管壁完全一致：用管壁同一套平滑半径（smoothedRadius），
        // 再往管内收 4mm —— 既避开多边形管壁的弦高，也避开管径抖动。
        var idxNear = 0, bestGap = Infinity;
        for (var q = 0; q < sections.length; q++) {
            var g = Math.abs((sections[q].mileage || 0) - dm);
            if (g < bestGap) { bestGap = g; idxNear = q; }
        }
        var r = smoothedRadius(idxNear);

        // 环向方位：right=0rad / left=π / top=π2（管道水平，up=+Y）
        var th0 = (d.wall_side === 'left') ? Math.PI
                : (d.wall_side === 'top') ? Math.PI / 2 : 0;
        var pw = Math.min(r * 0.80, 0.19);      // 轴向弧长
        var ph = pw * 0.75;                      // 环向弧长
        var dTheta = ph / r;                     // 环向张角
        var rr = Math.max(0.06, r - 0.004);      // 贴内壁，收 4mm
        var fwd0 = new THREE.Vector3(Math.cos(h), 0, -Math.sin(h));

        // 曲面贴片：沿（轴向 s × 环向 t）细分，逐点落在同一张管壁曲面上，
        // 因此左右两侧/顶部都不会像平面卡片那样把四角顶出管壁。
        function poseAt(mm) {
            if (tr && tr.fixed) { return trackPose(mm, tr); }
            return {
                pos: basePos.clone().add(fwd0.clone().multiplyScalar(mm - dm)),
                h: h
            };
        }
        var NI = 12, NJ = 3, verts = [], uvs = [], idx2 = [];
        var acc = new THREE.Vector3();
        for (var j = 0; j <= NJ; j++) {
            var s = j / NJ;                                  // 轴向 0..1
            var pp = poseAt(dm + (s - 0.5) * pw);
            var eRx = Math.sin(pp.h), eRz = Math.cos(pp.h);
            for (var i = 0; i <= NI; i++) {
                var t = i / NI;                              // 环向 0..1
                var th = th0 - dTheta / 2 + dTheta * t;
                var co = Math.cos(th), si = Math.sin(th);
                var px = pp.pos.x + eRx * co * rr;
                var py = pp.pos.y + si * rr;
                var pz = pp.pos.z + eRz * co * rr;
                acc.x += px; acc.y += py; acc.z += pz;
                verts.push(px, py, pz);
                // UV 朝向：照片上沿朝管顶（顶部展位朝行进方向），
                // 管内正视不镜像
                var u, v;
                if (d.wall_side === 'top') { u = t; v = s; }
                else if (d.wall_side === 'left') { u = s; v = t; }
                else { u = 1 - s; v = t; }
                uvs.push(u, v);
            }
        }
        for (var jj = 0; jj < NJ; jj++) {
            for (var ii = 0; ii < NI; ii++) {
                var a0 = jj * (NI + 1) + ii, b0 = a0 + 1,
                    c0 = a0 + NI + 1, d0 = c0 + 1;
                idx2.push(a0, c0, b0, b0, c0, d0);
            }
        }
        var vcount = verts.length / 3;
        acc.multiplyScalar(1 / vcount);          // 以贴片中心为局部原点
        for (var k = 0; k < vcount; k++) {
            verts[k * 3] -= acc.x;
            verts[k * 3 + 1] -= acc.y;
            verts[k * 3 + 2] -= acc.z;
        }
        var geo = new THREE.BufferGeometry();
        geo.setAttribute('position',
            new THREE.Float32BufferAttribute(verts, 3));
        geo.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
        geo.setIndex(idx2);
        geo.computeVertexNormals();

        var pm = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({
            // 整张后端标注图（自带类型/等级/里程信息条），不再裁检测框
            map: defectPhotoTexture(d),
            side: THREE.DoubleSide,              // 曲面正反都可见，绕序无关
            polygonOffset: true, polygonOffsetFactor: -2,
            polygonOffsetUnits: -2
        }));
        pm.position.copy(acc);                   // 顶点已是局部坐标，位移到贴壁位置
        pm.userData.isPhoto = true;
        group.add(pm);
        defectSprites.push(pm);
    }

    function buildDefect(d) {
        if (d.visible === false) { return; }
        var key = d.type + '@' + d.mileage;
        if (builtDefects[key]) { return; }
        builtDefects[key] = true;

        var sec = nearestSection(d.mileage);
        if (!sec) { return; }
        var r = (sec.diameter || 0.8) / 2;
        var pos = toVec(sec);
        var h = headingRadOf(sec);
        var rightX = Math.cos(h + Math.PI / 2), rightY = Math.sin(h + Math.PI / 2);

        // v8 固定展位（2026-10-27 用户拍板）：后端带 wall_side/display_mileage
        // 的缺陷（演示管 5 类）按实物布局锁死贴壁，不走针孔投影
        if (d.image && d.wall_side) {
            buildSlottedDefect(d, key);
            return;
        }

        if (d.image && d.bbox && d.bbox.w > 0) {
            // 有实拍 + 检测框 → 针孔投影定缺陷在管壁上的真实方位：
            // 相机位于管轴朝前，画面点 (bx,by) 的视线方向命中半径 r 的
            // 管壁于「轴向 zHit、环向 aHit/bHit」处，贴片裁缺陷区域贴上去。
            var MODEL = 256, tanH = Math.tan(50 * DEG);   // 假设水平视场 ~100°
            var bb = d.bbox;
            var dxc = (bb.x / MODEL * 2 - 1) * tanH;
            var dyc = (bb.y / MODEL * 2 - 1) * tanH;      // 画面 y 向下
            var rayLen = Math.sqrt(dxc * dxc + dyc * dyc);
            if (rayLen < 0.08) { dyc = -0.08; rayLen = Math.sqrt(dxc * dxc + dyc * dyc); }
            // 贴片必须落在已建成的管壁内：管子是随里程生长的，
            // 缺陷前方 zHit 超出已建里程时先不建（下轮轮询再试）
            var maxZ = (latestData && latestData.total_mileage
                        ? latestData.total_mileage - d.mileage : 0);
            if (maxZ < 0.12) { delete builtDefects[key]; return; }
            var zHit = Math.min(1.2, Math.max(0.12, Math.min(maxZ * 0.95, r / rayLen)));
            var aHit = r * dxc / rayLen;
            var bHit = r * dyc / rayLen;

            var fwd = new THREE.Vector3(Math.cos(h), 0, -Math.sin(h));
            var rgt = new THREE.Vector3(-Math.sin(h), 0, -Math.cos(h));
            var axisPt = pos.clone().add(fwd.multiplyScalar(zHit));
            var wallPt = axisPt.clone()
                .add(rgt.multiplyScalar(aHit))
                .add(new THREE.Vector3(0, -bHit, 0));   // 画面向下 = 场景向下

            // 贴片尺寸：角宽 ≈ 框宽占比 × 水平视场，保持快照像素纵横比
            var HFOV_RAD = 100 * DEG;
            var fracW = Math.min(1, (bb.w + 10) / MODEL);
            var fracH = Math.min(1, (bb.h + 10) / MODEL);
            var pw = Math.min(r * 1.15, r * Math.max(0.16, fracW * HFOV_RAD));
            var ph = Math.min(r * 1.15,
                     Math.max(r * 0.12, pw * (fracH * 720) / (fracW * 1280)));

            var pm = new THREE.Mesh(
                new THREE.PlaneGeometry(pw, ph),
                new THREE.MeshBasicMaterial({
                    map: defectPatchTexture(d),
                    polygonOffset: true, polygonOffsetFactor: -2,
                    polygonOffsetUnits: -2
                })
            );
            pm.position.copy(wallPt);
            pm.lookAt(axisPt);            // 面朝管轴，与管壁相切
            pm.userData.isPhoto = true;
            group.add(pm);
            defectSprites.push(pm);

            // 小号信息标签悬在缺陷上方（类型 / 等级 / 里程）
            var label = new THREE.Sprite(new THREE.SpriteMaterial({
                map: defectSpriteTexture(d), transparent: true, depthTest: false
            }));
            label.position.copy(wallPt)
                .add(new THREE.Vector3(0, ph * 0.75 + r * 0.10, 0));
            label.scale.set(r * 0.80, r * 0.35, 1);
            label.renderOrder = 20;
            group.add(label);
            defectSprites.push(label);
            return;
        }

        if (d.image) {
            // 旧巡检记录（无框坐标）→ 保留照片牌兜底（右侧内壁 25° 高度）
            var lift = Math.sin(25 * DEG), lateral = Math.cos(25 * DEG);
            var pc = pos.clone().add(new THREE.Vector3(
                rightX * lateral * r * 0.90,
                lift * r * 0.90,
                -rightY * lateral * r * 0.90));
            var pw = r * 0.85, ph = pw * 0.75;
            var pm = new THREE.Mesh(
                new THREE.PlaneGeometry(pw, ph),
                new THREE.MeshStandardMaterial({
                    map: defectPhotoTexture(d),
                    roughness: 0.85, metalness: 0.0,
                    emissive: 0x1a222e, emissiveIntensity: 0.5
                })
            );
            pm.position.copy(pc);
            pm.lookAt(pos);
            pm.userData.isPhoto = true;
            group.add(pm);
            defectSprites.push(pm);
            return;
        }

        // 无实拍（演示模式示例缺陷等）→ 保留等级文字钉
        var center = pos.clone().add(new THREE.Vector3(
            rightX * r * 0.30, r * 0.70, -rightY * r * 0.30));

        var sprite = new THREE.Sprite(new THREE.SpriteMaterial({
            map: defectSpriteTexture(d), transparent: true, depthTest: false
        }));
        sprite.position.copy(center);
        sprite.scale.set(r * 1.6, r * 0.70, 1);
        sprite.userData.d = sec.diameter || 0.8;   // 缺陷脉冲缩放的管径基准
        sprite.renderOrder = 20;
        group.add(sprite);
        defectSprites.push(sprite);
    }

    function rebuildFromData(d) {
        latestData = d;
        if (d.running) { sawRunning = true; }
        sections = d.sections || [];
        keyframes = d.keyframes || [];
        // 兼容旧索引：无 keyframes 字段但断面带 image（旧版按断面抓帧）
        if (!keyframes.length) {
            keyframes = sections.filter(function (s) { return s.image; })
                .map(function (s, i) {
                    return { seq: i + 1, mileage: s.mileage, image: s.image };
                });
        }

        if (sections.length !== tubeBuilt) { rebuildTube(); }   // 连续管壁
        while (builtRings < sections.length) {
            buildRing(builtRings);
            builtRings++;
        }
        // 关键帧不再贴墙（2026-10-18 用户拍板：墙上只贴"缺陷实拍图"，
        // 有缺陷才贴、没有不贴；关键帧仍由后端存档进巡检目录）
        (d.defects || []).forEach(buildDefect);

        if (d.pose) { robotPose = d.pose; }
        // 位姿吸附：首次进入视图 / 换新巡检时，机器人直接吸附到当前位姿，
        // 不从管道入口插值飞进来（历史回放的 loadHistory 早修过，实时漏了）
        if (needSnap && d.pose && d.folder) {
            smoothPos.copy(robotWorldPos());
            smoothHeading = headingRadOf(robotPose);
            if (sections.length) {
                curDiameter = sections[sections.length - 1].diameter || 0.8;
            }
            needSnap = false;
        }
        updateHud();
    }

    function resetCorridor() {
        while (group.children.length) {
            group.remove(group.children[0]);
        }
        if (tubeMesh) {
            tubeMesh.geometry.dispose();
            tubeMesh = null;
        }
        tubeBuilt = 0;
        builtRings = 0;
        lastRulerK = -1;
        builtDefects = {};
        defectSprites = [];
        keyframes = [];
        needSnap = true;      // 重建后机器人直接就位，不从旧位置飞过来
    }

    // ------------------------------------------------------------------
    // 相机与交互（默认管内跟车视角）
    // ------------------------------------------------------------------
    function robotWorldPos() {
        return new THREE.Vector3(robotPose.x, robotPose.z, -robotPose.y);
    }

    function forwardVec() {
        // 用平滑后的航向，与相机/机器人动画同步（1s 轮询的原始 yaw 会跳变）
        return new THREE.Vector3(Math.cos(smoothHeading), 0, -Math.sin(smoothHeading));
    }

    function updateCamera() {
        // 相机跟"平滑后"的机器人位置（与 mesh 同步），消除 1s 轮询位姿跳变
        var pivot = smoothPos.clone();
        var fwd = forwardVec();
        var d = curDiameter;
        var dist = follow.dist * d;
        // 车后方向绕 Y 轴旋转 az，抬升 el
        var back = fwd.clone().negate();
        var ca = Math.cos(follow.az), sa = Math.sin(follow.az);
        var dir = new THREE.Vector3(
            back.x * ca - back.z * sa,
            Math.sin(follow.el),
            back.x * sa + back.z * ca
        );
        camera.position.copy(pivot).add(dir.multiplyScalar(dist));
        var look = pivot.clone().add(fwd.clone().multiplyScalar(d * 4.5));
        look.y += d * 0.10;
        camera.lookAt(look);
    }

    function bindControls() {
        var dom = renderer.domElement;
        var dragging = false, lastX = 0, lastY = 0;

        dom.addEventListener('pointerdown', function (e) {
            dragging = true; lastX = e.clientX; lastY = e.clientY;
            dom.setPointerCapture(e.pointerId);
        });
        dom.addEventListener('pointermove', function (e) {
            if (!dragging) { return; }
            follow.az -= (e.clientX - lastX) * 0.008;
            follow.el = Math.min(1.2, Math.max(-0.5,
                follow.el - (e.clientY - lastY) * 0.006));
            lastX = e.clientX; lastY = e.clientY;
        });
        dom.addEventListener('pointerup', function () { dragging = false; });
        dom.addEventListener('wheel', function (e) {
            e.preventDefault();
            follow.dist = Math.min(40, Math.max(0.7,
                follow.dist + e.deltaY * 0.012));
        }, { passive: false });
        dom.addEventListener('dblclick', function () {
            follow.az = FOLLOW_DEFAULT.az;
            follow.el = FOLLOW_DEFAULT.el;
            follow.dist = FOLLOW_DEFAULT.dist;
        });
    }

    function resize() {
        if (!renderer || !container) { return; }
        var w = container.clientWidth, h = container.clientHeight;
        if (w < 2 || h < 2) { return; }
        renderer.setSize(w, h, false);
        camera.aspect = w / h;
        camera.updateProjectionMatrix();
    }

    // ------------------------------------------------------------------
    // 渲染循环（30fps 节流）
    // ------------------------------------------------------------------
    function animate(ts) {
        if (!active) { rafId = null; return; }
        rafId = requestAnimationFrame(animate);
        if (ts - lastRenderTs < 33) { return; }
        lastRenderTs = ts;

        // 机器人位置平滑逼近最新位姿
        var target = robotWorldPos();
        smoothPos.lerp(target, 0.08);
        var h = headingRadOf(robotPose);
        var dh = h - smoothHeading;
        while (dh > Math.PI) { dh -= 2 * Math.PI; }
        while (dh < -Math.PI) { dh += 2 * Math.PI; }
        smoothHeading += dh * 0.08;

        // 管径平滑（比例基准），机器人按管径缩放（车长 ≈ 0.55 管径）
        var latestD = sections.length ?
            (sections[sections.length - 1].diameter || 0.8) : 0.8;
        curDiameter += (latestD - curDiameter) * 0.06;
        var k = curDiameter * 0.55;
        robotCarrier.scale.set(k, k, k);

        robotCarrier.position.copy(smoothPos);
        robotCarrier.position.y -= curDiameter * 0.28;   // 贴底行驶
        var pitchRad = (robotPose.pitch || 0) * DEG;
        robotCarrier.quaternion.setFromEuler(new THREE.Euler(
            -pitchRad, Math.atan2(Math.cos(smoothHeading), -Math.sin(smoothHeading)), 0, 'YXZ'
        ));

        // 车灯跟随（照射距离随管径）
        var fwd = forwardVec();
        var head = smoothPos.clone().add(fwd.clone().multiplyScalar(0.22 * k));
        head.y += curDiameter * 0.16;
        spotLight.position.copy(head);
        spotTarget.position.copy(
            smoothPos.clone().add(fwd.clone().multiplyScalar(curDiameter * 5)));
        glowLight.position.copy(head);

        // 水下悬浮颗粒：局部系跟随机器人，缓沉 + 摇摆 + 行进时相对水流后漂
        if (particlePts) {
            particleGroup.position.copy(smoothPos);
            particleGroup.quaternion.setFromUnitVectors(FORWARD_Z, fwd);
            var arr = particlePts.geometry.attributes.position.array;
            var flowing = latestData && latestData.moving;
            for (var pi = 0; pi < PARTICLE_N; pi++) {
                var bi = pi * 3;
                arr[bi + 1] -= 0.0005 * particleSpeeds[pi];         // 缓沉
                arr[bi] += Math.sin(ts * 0.0009 + pi) * 0.0004;     // 轻微摇摆
                if (flowing) { arr[bi + 2] -= 0.005; }              // 行进时后漂
                if (arr[bi + 1] < -0.25) { arr[bi + 1] = 0.25; }
                if (arr[bi + 2] < -0.4) { arr[bi + 2] += 2.0; }
            }
            particlePts.geometry.attributes.position.needsUpdate = true;
        }

        // 车灯体积光锥：apex 在灯位、向车头前方展开，随管径缩放
        // （v6 收敛：长 1.6d/底半径 0.48d/透明度 0.07，此前 2.6d 大锥在管外
        //  视角下像一个巨大半透明球包住车头）
        if (lightCone) {
            var coneL = curDiameter * 1.6;
            lightCone.position.copy(head);
            lightCone.quaternion.setFromUnitVectors(CONE_AXIS, fwd);
            lightCone.scale.set(coneL * 0.30, coneL, coneL * 0.30);
        }

        // 雾距随实测管径缩放（小管更浓的水下纵深；v7 收紧 far：前方更快隐入黑暗）
        scene.fog.near = curDiameter * 1.2;
        scene.fog.far = curDiameter * 8.0;

        // 缺陷脉冲（文字钉=原脉冲；照片牌=轻微呼吸，不闪透明度防照片发虚）
        var pulse = 1 + 0.06 * Math.sin(ts * 0.004);
        for (var i = 0; i < defectSprites.length; i++) {
            var s = defectSprites[i];
            if (s.userData.isPhoto) {
                var pb = 1 + 0.03 * Math.sin(ts * 0.004);
                s.scale.set(pb, pb, 1);
            } else {
                var base = s.userData.d || curDiameter;
                s.scale.set(base * 1.6 * pulse, base * 0.70 * pulse, 1);
                s.material.opacity = 0.78 + 0.22 * Math.sin(ts * 0.004);
            }
        }

        updateCamera();
        renderer.render(scene, camera);
    }

    // ------------------------------------------------------------------
    // 轮询
    // ------------------------------------------------------------------
    function poll() {
        if (!active || replayMode) { return; }
        fetch('/api/corridor/current', { cache: 'no-store' })
            .then(function (r) { return r.json(); })
            .then(function (d) {
                if (!active) { return; }
                // 换了新巡检目录 -> 整个走廊重建
                if (d.folder && latestData && latestData.folder && d.folder !== latestData.folder) {
                    resetCorridor();
                }
                if (!d.folder && latestData && latestData.folder) {
                    resetCorridor();   // 巡检结束，清空待下次
                }
                rebuildFromData(d);
            })
            .catch(function () { /* 网络抖动忽略 */ });
    }

    // ------------------------------------------------------------------
    // 对外接口
    // ------------------------------------------------------------------
    window.CorridorView = {
        enter: function () {
            if (!renderer && !init()) { return; }
            if (active) { return; }
            active = true;
            replayMode = false;      // 进入视图默认实时模式
            needSnap = true;          // 进视图直接吸附到位姿，不从入口飞入
            container.classList.remove('hidden');
            resize();
            poll();
            pollTimer = setInterval(poll, POLL_MS);
            rafId = requestAnimationFrame(animate);
        },
        exit: function () {
            active = false;
            container.classList.add('hidden');
            if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
            if (rafId) { cancelAnimationFrame(rafId); rafId = null; }
        },
        isActive: function () { return active; },
        // 历史巡检回放：加载落盘的 corridor_index.json 重建整条走廊
        loadHistory: function (folder) {
            if (!renderer && !init()) { return; }
            fetch('/api/corridor/history/' + encodeURIComponent(folder))
                .then(function (r) {
                    if (!r.ok) { throw new Error('no corridor data'); }
                    return r.json();
                })
                .then(function (d) {
                    if (!d.sections || !d.sections.length) {
                        throw new Error('empty');
                    }
                    replayMode = true;
                    replayFolder = folder;
                    resetCorridor();
                    var last = d.sections[d.sections.length - 1];
                    rebuildFromData({
                        folder: folder,
                        running: false,
                        moving: false,
                        total_mileage: d.total_mileage || last.mileage || 0,
                        demo_mode: false,
                        sections: d.sections,
                        keyframes: d.keyframes || [],
                        defects: d.defects || [],
                        track: d.track || null,
                        pose: {
                            x: last.x, y: last.y, z: last.z,
                            yaw: last.yaw || 0, pitch: last.pitch || 0,
                        },
                    });
                    // 机器人直接停在终点（不插值漂移）
                    smoothPos.copy(robotWorldPos());
                    smoothHeading = headingRadOf(robotPose);
                })
                .catch(function () {
                    replayMode = false;
                    latestData = latestData || { folder: null };
                    updateHud();
                    var tag = hudEls.mode && hudEls.mode.querySelector('#corridor-mode-tag');
                    if (tag) {
                        tag.textContent = '该巡检无走廊数据';
                        tag.style.background = '#3d2323'; tag.style.color = '#ff9b9b';
                    }
                });
        },
    };
})();
