// ============================================================================
// prelude.js —— 动态 app 标准库（runtime 自动在业务脚本之前 eval 一次）
//
// 暴露给业务脚本的全局：
//   VDOM     —— 声明式 UI 框架，含 h / mount / find / set / destroy /
//               dispatch / render（带 diff）
//   h        —— VDOM.h 的短名
//   makeBle  —— BLE 路由 helper 工厂；业务调 makeBle("myapp") 拿到
//               { send, on, onAny, onError, isConnected, appName }
//
// 内部副作用：
//   - 注册 sys.__setDispatcher(VDOM.dispatch)，业务无需关心事件分发
//
// 约束：esp-mquickjs 仅支持 ES5；同一函数作用域内多个 catch 不能用同名标识符。
// ============================================================================

// ---------------------------- VDOM ------------------------------------------

var VDOM = (function () {

    var nodes = {};
    var autoSeq = 0;

    var FONT_MAP  = { text: sys.font.TEXT, title: sys.font.TITLE, huge: sys.font.HUGE,
                      icon24: sys.font.ICON_24, icon36: sys.font.ICON_36, numM: sys.font.NUM_M };
    var ALIGN_MAP = {
        tl: sys.align.TOP_LEFT,    tm: sys.align.TOP_MID,    tr: sys.align.TOP_RIGHT,
        lm: sys.align.LEFT_MID,    c:  sys.align.CENTER,     rm: sys.align.RIGHT_MID,
        bl: sys.align.BOTTOM_LEFT, bm: sys.align.BOTTOM_MID, br: sys.align.BOTTOM_RIGHT
    };
    /* flex align: start/end/center/sevenly/around/between */
    var FLEX_ALIGN_MAP = { start: 0, end: 1, center: 2, evenly: 3, around: 4, between: 5 };
    /* label long mode: wrap/dot/scroll/clip */
    var LONG_MODE_MAP  = { wrap: 0, dot: 1, scroll: 2, clip: 3 };
    /* text align: left/center/right */
    var TEXT_ALIGN_MAP = { left: 0, center: 1, right: 2 };

    function h(type, props, children) {
        return {
            type: type,
            props: props || {},
            children: children || [],
            _parent: null,
            _mounted: false
        };
    }

    function autoId(type) {
        autoSeq += 1;
        return "_" + type + "_" + autoSeq;
    }

    function applyStyle(id, props) {
        if (props.bg !== undefined)
            sys.ui.setStyle(id, sys.style.BG_COLOR, props.bg, 0, 0, 0);
        if (props.fg !== undefined)
            sys.ui.setStyle(id, sys.style.TEXT_COLOR, props.fg, 0, 0, 0);
        if (props.radius !== undefined)
            sys.ui.setStyle(id, sys.style.RADIUS, props.radius, 0, 0, 0);
        if (props.size !== undefined)
            sys.ui.setStyle(id, sys.style.SIZE,
                props.size[0] | 0, props.size[1] | 0, 0, 0);
        if (props.align !== undefined) {
            var a = props.align;
            var atype = ALIGN_MAP[a[0]];
            if (atype === undefined) atype = sys.align.CENTER;
            sys.ui.setStyle(id, sys.style.ALIGN,
                atype, a[1] | 0, a[2] | 0, 0);
        }
        if (props.pad !== undefined) {
            var p = props.pad;
            sys.ui.setStyle(id, sys.style.PAD,
                p[0] | 0, p[1] | 0, p[2] | 0, p[3] | 0);
        }
        if (props.borderBottom !== undefined)
            sys.ui.setStyle(id, sys.style.BORDER_BOTTOM, props.borderBottom, 0, 0, 0);
        if (props.flex !== undefined)
            sys.ui.setStyle(id, sys.style.FLEX,
                props.flex === 'row' ? 1 : 0, 0, 0, 0);
        if (props.font !== undefined) {
            var f = FONT_MAP[props.font];
            if (f === undefined) f = sys.font.TEXT;
            sys.ui.setStyle(id, sys.style.FONT, f, 0, 0, 0);
        }
        if (props.shadow !== undefined) {
            var sh = props.shadow;
            sys.ui.setStyle(id, sys.style.SHADOW,
                sh[0] | 0, sh[1] | 0, sh[2] | 0, 0);
        }
        if (props.gap !== undefined) {
            var g = props.gap;
            sys.ui.setStyle(id, sys.style.GAP,
                g[0] | 0, g[1] | 0, 0, 0);
        }
        if (props.scrollable !== undefined) {
            sys.ui.setStyle(id, sys.style.SCROLLABLE,
                props.scrollable ? 1 : 0, 0, 0, 0);
        }
        if (props.opa !== undefined)
            sys.ui.setStyle(id, sys.style.OPA, props.opa | 0, 0, 0, 0);
        if (props.bgOpa !== undefined)
            sys.ui.setStyle(id, sys.style.BG_OPA, props.bgOpa | 0, 0, 0, 0);
        if (props.hidden !== undefined)
            sys.ui.setStyle(id, sys.style.HIDDEN, props.hidden ? 1 : 0, 0, 0, 0);
        if (props.grow !== undefined)
            sys.ui.setStyle(id, sys.style.FLEX_GROW, props.grow | 0, 0, 0, 0);
        if (props.textAlign !== undefined) {
            var ta = TEXT_ALIGN_MAP[props.textAlign];
            if (ta === undefined) ta = 0;
            sys.ui.setStyle(id, sys.style.TEXT_ALIGN, ta, 0, 0, 0);
        }
        if (props.longMode !== undefined) {
            var lm = LONG_MODE_MAP[props.longMode];
            if (lm === undefined) lm = 0;
            sys.ui.setStyle(id, sys.style.LONG_MODE, lm, 0, 0, 0);
        }
        if (props.rotate !== undefined) {
            /* rotate: number(0.1°) | [angle, pivotX, pivotY] */
            var r = props.rotate;
            if (typeof r === 'number') {
                sys.ui.setStyle(id, sys.style.ROTATION, r | 0, 0, 0, 0);
            } else {
                sys.ui.setStyle(id, sys.style.ROTATION,
                    r[0] | 0, r[1] | 0, r[2] | 0, 0);
            }
        }
        if (props.flexAlign !== undefined) {
            /* flexAlign: [main, cross, track] (string keys) */
            var fa = props.flexAlign;
            var m = FLEX_ALIGN_MAP[fa[0]]; if (m === undefined) m = 0;
            var x = FLEX_ALIGN_MAP[fa[1]]; if (x === undefined) x = 0;
            var t = FLEX_ALIGN_MAP[fa[2]]; if (t === undefined) t = 0;
            sys.ui.setStyle(id, sys.style.FLEX_ALIGN, m, x, t, 0);
        }
        if (props.border !== undefined) {
            /* border: { color, width, side?, opa? }
             * side: 'full'|'top'|'bottom'|'left'|'right' (默认 full) */
            var bd = props.border;
            var sb = 0x10;  /* full */
            if      (bd.side === 'top')    sb = 0x01;
            else if (bd.side === 'bottom') sb = 0x02;
            else if (bd.side === 'left')   sb = 0x04;
            else if (bd.side === 'right')  sb = 0x08;
            sys.ui.setStyle(id, sys.style.BORDER,
                bd.color | 0, (bd.width || 1) | 0, sb,
                bd.opa === undefined ? 255 : (bd.opa | 0));
        }
        if (props.pressedBg !== undefined) {
            /* pressedBg: { color, opa? } */
            var pb = props.pressedBg;
            sys.ui.setStyle(id, sys.style.PRESSED_BG,
                pb.color | 0,
                pb.opa === undefined ? 0 : (pb.opa | 0), 0, 0);
        }
    }

    var HOOK_NAME = {
        1: 'onClick', 2: 'onPress', 3: 'onDrag',
        4: 'onRelease', 5: 'onLongPress'
    };
    /* modal 回调注册表：sys.ui.modal 触发时 dispatcher 会查这里。
     * key = modal_id 字符串（与 C 端 EV_MODAL.node_id 一致） */
    var modalCbs = {};
    var modalSeq = 1;

    function dispatch(startId, type, dx, dy) {
        /* type=6 是 EV_MODAL：startId 是 modal_id，dx 是按钮索引（-1=取消） */
        if (type === 6) {
            var cb = modalCbs[startId];
            if (cb) {
                delete modalCbs[startId];
                try { cb(dx | 0); }
                catch (eM) { sys.log('modal cb throw: ' + eM); }
            }
            return;
        }
        var node = nodes[startId];
        if (!node) return;
        var hook = HOOK_NAME[type];
        if (!hook) return;
        var stopped = false;
        var ev = {
            target: startId, currentTarget: null,
            dx: dx | 0, dy: dy | 0,
            stopPropagation: function () { stopped = true; }
        };
        var cur = node;
        while (cur) {
            if (typeof cur.props[hook] === 'function') {
                ev.currentTarget = cur.props.id;
                var ret = cur.props[hook](ev);
                if (ret === false || stopped) return;
            }
            cur = cur._parent;
        }
    }

    /* 给业务用的 modal 入口：UI.modal({...}) 内部调，VDOM 自管 id 和 cb 注册。
     * 业务一般通过 UI.modal 调用，不直接用 VDOM.showModal。 */
    function showModal(opts) {
        var id = modalSeq++;
        if (typeof opts.onResult === 'function') {
            modalCbs['' + id] = opts.onResult;
        }
        sys.ui.modal({
            id: id,
            title:   opts.title   || '',
            body:    opts.body    || '',
            action0: opts.action0 || '',
            action1: opts.action1 || ''
        });
        return id;
    }

    function mount(node, parentId) {
        if (node._mounted) return node;
        var id = node.props.id;
        if (!id) { id = autoId(node.type); node.props.id = id; }

        if (node.type === 'panel')       sys.ui.createPanel(id, parentId || null);
        else if (node.type === 'button') sys.ui.createButton(id, parentId || null);
        else if (node.type === 'label')  sys.ui.createLabel(id, parentId || null);
        else if (node.type === 'image')  sys.ui.createImage(id, parentId || null,
                                                            node.props.src || null);
        else if (node.type === 'canvas') sys.canvas.create(id, parentId || null,
                                                            (node.props.w || 0) | 0,
                                                            (node.props.h || 0) | 0);
        else { sys.log("VDOM: unknown type " + node.type); return node; }

        if (node.props.text !== undefined) sys.ui.setText(id, "" + node.props.text);
        applyStyle(id, node.props);

        nodes[id] = node;
        node._mounted = true;

        for (var i = 0; i < node.children.length; i++) {
            node.children[i]._parent = node;
            mount(node.children[i], id);
        }
        return node;
    }

    function find(id) { return nodes[id] || null; }

    function set(id, patch) {
        var node = nodes[id];
        if (!node) { sys.log("VDOM.set: id not found: " + id); return; }
        for (var k in patch) {
            if (!patch.hasOwnProperty(k)) continue;
            node.props[k] = patch[k];
        }
        if (patch.text !== undefined) sys.ui.setText(id, "" + patch.text);
        if (patch.src  !== undefined) sys.ui.setImageSrc(id, patch.src || null);
        applyStyle(id, patch);
    }

    function destroy(id) {
        var node = nodes[id];
        if (!node) return;
        var kids = node.children.slice();
        for (var i = 0; i < kids.length; i++) {
            if (kids[i].props && kids[i].props.id) destroy(kids[i].props.id);
        }
        var parent = node._parent;
        if (parent && parent.children) {
            var idx = parent.children.indexOf(node);
            if (idx >= 0) parent.children.splice(idx, 1);
        }
        sys.ui.destroy(id);
        delete nodes[id];
    }

    var STYLE_KEYS = ['bg', 'fg', 'radius', 'size', 'align',
                      'pad', 'borderBottom', 'flex', 'font',
                      'shadow', 'gap', 'scrollable',
                      'opa', 'bgOpa', 'grow', 'textAlign', 'longMode',
                      'rotate', 'flexAlign', 'border', 'pressedBg', 'hidden'];

    function shallowEq(a, b) {
        if (a === b) return true;
        if (a == null || b == null) return false;
        if (typeof a !== 'object' || typeof b !== 'object') return false;
        if (a.length === undefined || b.length === undefined) return false;
        if (a.length !== b.length) return false;
        for (var i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
        return true;
    }

    function diffProps(oldNode, newNode) {
        var id = oldNode.props.id;
        var oldP = oldNode.props;
        var newP = newNode.props;
        var patch = {};
        var hasPatch = false;

        if (newP.text !== undefined && newP.text !== oldP.text) {
            sys.ui.setText(id, "" + newP.text);
        }
        if (newP.src !== undefined && newP.src !== oldP.src) {
            sys.ui.setImageSrc(id, newP.src || null);
        }
        for (var i = 0; i < STYLE_KEYS.length; i++) {
            var k = STYLE_KEYS[i];
            if (newP[k] !== undefined && !shallowEq(oldP[k], newP[k])) {
                patch[k] = newP[k];
                hasPatch = true;
            }
        }
        if (hasPatch) applyStyle(id, patch);

        newP.id = id;
        oldNode.props = newP;
    }

    function diffNode(oldNode, newNode, parentId) {
        var id = oldNode.props.id;
        if (oldNode.type !== newNode.type) {
            var parent = oldNode._parent;
            destroy(id);
            newNode._parent = parent;
            mount(newNode, parentId);
            return newNode;
        }
        diffProps(oldNode, newNode);
        diffChildren(oldNode, newNode, id);
        return oldNode;
    }

    function diffChildren(oldNode, newNode, parentId) {
        var oldKids = oldNode.children;
        var newKids = newNode.children || [];

        var oldById = {};
        for (var i = 0; i < oldKids.length; i++) {
            var oid = oldKids[i].props.id;
            if (oid) oldById[oid] = oldKids[i];
        }

        var resultKids = [];
        var seen = {};
        for (i = 0; i < newKids.length; i++) {
            var nk = newKids[i];
            var nid = nk.props.id;
            if (nid && oldById[nid]) {
                seen[nid] = true;
                resultKids.push(diffNode(oldById[nid], nk, parentId));
            } else {
                nk._parent = oldNode;
                mount(nk, parentId);
                resultKids.push(nk);
            }
        }

        var toRemove = [];
        for (i = 0; i < oldKids.length; i++) {
            var rid = oldKids[i].props.id;
            if (rid && !seen[rid]) toRemove.push(rid);
        }
        for (i = 0; i < toRemove.length; i++) destroy(toRemove[i]);

        oldNode.children = resultKids;
    }

    function render(rootDesc, parentId) {
        if (!rootDesc || !rootDesc.props || !rootDesc.props.id) {
            sys.log("VDOM.render: root must have id");
            return null;
        }
        var existing = nodes[rootDesc.props.id];
        if (!existing) return mount(rootDesc, parentId || null);
        diffNode(existing, rootDesc, parentId || null);
        return existing;
    }

    return {
        h: h, mount: mount, find: find, set: set, destroy: destroy,
        dispatch: dispatch, render: render,
        showModal: showModal
    };
})();

var h = VDOM.h;

sys.__setDispatcher(function (id, type, dx, dy) {
    VDOM.dispatch(id, type, dx, dy);
});

// ---------------------------- BLE helper ------------------------------------
//
// 用法:
//   var ble = makeBle("myapp");
//   ble.on("data", function (msg) { sys.log(JSON.stringify(msg.body)); });
//   ble.send("req", { force: true });
//
// 返回对象:
//   send(type, body?)   -> bool   发 { from: appName, type, body? } 给 PC
//   on(type, fn)                  注册 type 回调；fn(body, meta) 直接收 body
//                                 meta = { type, from }（极少用，备查）
//   onAny(fn)                     收任何"给我的"消息（不含 ping）；fn(msg) 收完整字典
//   onError(fn)                   JSON 解析失败回调；fn 收 raw 字符串
//   isConnected()        -> bool
//   appName              字符串
//
// 行为约定:
//   - 每个 app 调用一次 makeBle 即可；新调用会覆盖旧的底层 onRecv
//   - "to" 字段不匹配 appName 且不为 "*" 的消息会被静默丢弃
//   - type === "ping" 自动回 pong，业务侧 on("ping") 不会被触发
//   - body 缺失时 fn 收到 {} 而不是 undefined（少一层 if 判断）
// ----------------------------------------------------------------------------

function makeBle(appName) {
    var typeRoutes = {};
    var anyHandler = null;
    var errorHandler = null;

    sys.ble.onRecv(function (raw) {
        var msg;
        try { msg = JSON.parse(raw); }
        catch (eParse) {
            if (errorHandler) {
                try { errorHandler(raw); } catch (_) {}
            }
            return;
        }
        if (!msg || (msg.to !== appName && msg.to !== "*")) return;

        if (msg.type === "ping") {
            var reply = { from: appName, type: "pong" };
            if (msg.body !== undefined) reply.body = msg.body;
            sys.ble.send(JSON.stringify(reply));
            return;
        }

        if (anyHandler) {
            try { anyHandler(msg); }
            catch (eAny) { sys.log("ble.onAny throw: " + eAny); }
        }
        if (msg.type && typeRoutes[msg.type]) {
            var body = (msg.body !== undefined && msg.body !== null) ? msg.body : {};
            var meta = { type: msg.type, from: msg.from };
            try { typeRoutes[msg.type](body, meta); }
            catch (eType) { sys.log("ble.on(" + msg.type + ") throw: " + eType); }
        }
    });

    return {
        appName: appName,
        send: function (type, body) {
            var msg = { from: appName, type: type };
            if (body !== undefined) msg.body = body;
            return sys.ble.send(JSON.stringify(msg));
        },
        on:        function (t, fn) { typeRoutes[t] = fn; },
        onAny:     function (fn)    { anyHandler = fn; },
        onError:   function (fn)    { errorHandler = fn; },
        isConnected: sys.ble.isConnected
    };
}

// ============================================================================
// UI —— P0 设计系统：iOS 浅色风格高阶组件
//
// 仿照 app/ui/ui_widgets.c + ui_tokens.h，把"卡片/列表行/键值行/图标按钮/模态"
// 等原生 app 通用控件用 VDOM 节点封装。脚本侧只用语义 token (UI.T.C_*) 和
// 组件 (UI.card / UI.listRow ...) 就能搭出"和原生 app 看起来一致"的页面。
//
// 用法:
//   var page = UI.screen('p', [
//       UI.title('蓝牙'),
//       UI.card({ pad: [0,0,0,0] }, [
//           UI.listRow({ icon: sys.icons.BLUETOOTH, label: '蓝牙',
//                        value: '已连接', iconBg: UI.T.C_ACCENT }),
//       ])
//   ]);
//   VDOM.mount(page, null);
//   sys.ui.attachRootListener('p');
// ============================================================================

var UI = (function () {

    var T = sys.tokens;
    var I = sys.icons;
    /* SIZE_CONTENT：宽/高按子内容自适应（LV_SIZE_CONTENT 的 sentinel）。
     * 数值与 dynamic_app_ui_styles.c::resolve_size 和 sys.size.CONTENT 一致。 */
    var SIZE_CONTENT = (sys.size && sys.size.CONTENT !== undefined) ? sys.size.CONTENT : -32768;

    // ---- screen: 屏幕底容器（iOS 灰白底，全屏）-----------------------------
    function screen(id, children) {
        return h('panel', {
            id: id, size: [-100, -100], bg: T.C_BG, scrollable: false,
            pad: [0, 0, 0, 0]
        }, children || []);
    }

    // ---- title: 页面大标题（左对齐 16px CJK，黑字）-------------------------
    function title(text, props) {
        var p = props || {};
        var merged = {
            text: text, fg: T.C_TEXT, font: 'title',
            pad: [T.SP_LG, T.SP_MD, T.SP_LG, 0]
        };
        for (var k in p) if (p.hasOwnProperty(k)) merged[k] = p[k];
        return h('label', merged);
    }

    // ---- card: 白底卡片 + 1px 浅描边 + 圆角 14 -----------------------------
    // opts.pad 默认 [12,12,12,12]；传 [0,0,0,0] 让里面的 listRow 自管 padding
    // opts.size 默认 [-100, SIZE_CONTENT]（撑满父宽、按子内容自适应高度）。
    // opts.flex 默认 'col'：iOS 卡片就是"垂直堆叠列表行/控件"，不开 column flex
    // 多个子对象会全部堆在 (0,0) 重叠，只能看到最顶上那个。
    // 不要不传 size —— 当父是 column flex 时，未设宽会让子的 lv_pct(100) 反向
    // 拉成 0 宽，整张卡片被压成中央一小条（settings_pkg 曾因此 layout 错乱）。
    function card(opts, children) {
        var o = opts || {};
        var props = {
            id: o.id,
            bg: T.C_PANEL,
            radius: T.R_LG,
            border: { color: T.C_BORDER, width: 1, side: 'full', opa: 128 },
            pad: o.pad === undefined ? [T.SP_MD, T.SP_MD, T.SP_MD, T.SP_MD] : o.pad,
            scrollable: false,
            size: o.size === undefined ? [-100, SIZE_CONTENT] : o.size,
            flex: o.flex === undefined ? 'col' : o.flex
        };
        if (o.align !== undefined)     props.align     = o.align;
        if (o.gap !== undefined)       props.gap       = o.gap;
        if (o.flexAlign !== undefined) props.flexAlign = o.flexAlign;
        return h('panel', props, children || []);
    }

    // ---- kvRow: 左右两端 key/value 行（无图标）----------------------------
    // opts: { key, value, valueId?, divider? (true), id? }
    function kvRow(opts) {
        var o = opts || {};
        var children = [
            h('label', { text: o.key || '', fg: T.C_TEXT_MUTED, font: 'text' }),
            h('label', { text: o.value === undefined ? '--' : ('' + o.value),
                         fg: T.C_TEXT, font: 'text', id: o.valueId })
        ];
        var props = {
            size: [-100, 36],
            flex: 'row',
            flexAlign: ['between', 'center', 'center'],
            pad: [T.SP_MD, T.SP_SM, T.SP_MD, T.SP_SM]
        };
        if (o.divider !== false) {
            props.border = { color: T.C_BORDER, width: 1, side: 'bottom', opa: 102 };
        }
        if (o.id) props.id = o.id;
        return h('panel', props, children);
    }

    // ---- listRow: iOS 列表行（图标 + 标签 + 可选值 + chevron）-------------
    // opts: { icon, label, value?, valueId?, iconBg?, iconColor?,
    //         onClick?, divider? (true), id? }
    function listRow(opts) {
        var o = opts || {};
        var iconBg = o.iconBg === undefined ? T.C_TEXT_MUTED : o.iconBg;
        var iconFg = o.iconColor === undefined ? T.C_PANEL : o.iconColor;

        var iconBlock = h('panel', {
            size: [28, 28], bg: iconBg, radius: 6, scrollable: false
        }, [
            h('label', { text: o.icon || '', fg: iconFg, font: 'icon24',
                         align: ['c', 0, 0] })
        ]);

        var children = [iconBlock];
        children.push(h('label', { text: o.label || '', fg: T.C_TEXT,
                                   font: 'text', grow: 1, longMode: 'dot' }));
        if (o.value !== undefined && o.value !== null) {
            children.push(h('label', { text: '' + o.value,
                                       fg: T.C_TEXT_MUTED, font: 'text',
                                       id: o.valueId }));
        }
        children.push(h('label', { text: I.CHEVRON_RIGHT,
                                   fg: T.C_TEXT_MUTED, font: 'icon24' }));

        var props = {
            size: [-100, 48],
            flex: 'row',
            flexAlign: ['start', 'center', 'center'],
            gap: [0, T.SP_MD],
            pad: [T.SP_MD, 0, T.SP_MD, 0],
            scrollable: false,
            pressedBg: { color: T.C_PANEL_HI, opa: 255 }
        };
        if (o.divider !== false) {
            props.border = { color: T.C_BORDER, width: 1, side: 'bottom', opa: 102 };
        }
        if (o.id) props.id = o.id;
        if (o.onClick) props.onClick = o.onClick;

        // 用 button 而不是 panel：天然 PRESSED 视觉态
        return h('button', props, children);
    }

    // ---- iconBtn: 透明图标按钮（按下 accent 蒙层）-------------------------
    // opts: { id, icon, iconId?, color?, w?, h?, onClick }
    function iconBtn(opts) {
        var o = opts || {};
        var color = o.color === undefined ? T.C_ACCENT : o.color;
        return h('button', {
            id: o.id,
            size: [o.w || 36, o.h || 30],
            radius: T.R_MD,
            pad: [T.SP_XS, T.SP_XS, T.SP_XS, T.SP_XS],
            pressedBg: { color: T.C_ACCENT, opa: 51 },
            onClick: o.onClick
        }, [
            h('label', { id: o.iconId, text: o.icon || '', fg: color,
                         font: 'icon24', align: ['c', 0, 0] })
        ]);
    }

    // ---- pillBtn: 填充胶囊按钮（accent 蓝底白字）--------------------------
    // opts: { id, text, textId?, w?, h?, bg?, fg?, onClick }
    //   textId: 给内部 label 一个 id，业务可用 VDOM.set(textId, {text:'...'}) 切换文字
    function pillBtn(opts) {
        var o = opts || {};
        return h('button', {
            id: o.id,
            size: [o.w || 140, o.h || 44],
            bg: o.bg === undefined ? T.C_ACCENT : o.bg,
            radius: T.R_MD,
            pressedBg: { color: 0x000000, opa: 38 },
            onClick: o.onClick
        }, [
            h('label', { id: o.textId, text: o.text || '',
                         fg: o.fg === undefined ? T.C_PANEL : o.fg,
                         font: 'title', align: ['c', 0, 0] })
        ]);
    }

    // ---- badge: 圆角胶囊小标签（数字徽章 / tag）---------------------------
    // opts: { id, text, textId?, w?, h?, bg?, fg? }
    function badge(opts) {
        var o = opts || {};
        return h('panel', {
            id: o.id,
            size: [o.w || 36, o.h || 20],
            bg: o.bg === undefined ? T.C_ACCENT : o.bg,
            radius: T.R_PILL,
            scrollable: false
        }, [
            h('label', { id: o.textId,
                         text: o.text === undefined ? '' : ('' + o.text),
                         fg: o.fg === undefined ? T.C_PANEL : o.fg,
                         font: 'text', align: ['c', 0, 0] })
        ]);
    }

    // ---- statusBar: 仿原生顶栏（左 title，右图标占位）---------------------
    // opts: { title, titleId?, right, rightId?, compact? }
    //   compact=true：高 28、上下 pad 0（小屏内容稠密的 app 用）
    //   titleId / rightId：业务想动态改标题/右图标时用
    function statusBar(opts) {
        var o = opts || {};
        var compact = !!o.compact;
        var children = [
            h('label', { id: o.titleId,
                         text: o.title || '', fg: T.C_TEXT, font: 'title',
                         grow: 1, longMode: 'dot' })
        ];
        if (o.right) {
            children.push(h('label', { id: o.rightId,
                                       text: o.right, fg: T.C_TEXT_MUTED,
                                       font: 'icon24' }));
        }
        return h('panel', {
            size: [-100, compact ? 28 : 44],
            flex: 'row',
            flexAlign: ['between', 'center', 'center'],
            pad: compact
                ? [T.SP_MD, 0, T.SP_MD, 0]
                : [T.SP_LG, T.SP_SM, T.SP_LG, T.SP_SM],
            scrollable: false
        }, children);
    }

    // ---- divider: 水平分隔线 -----------------------------------------------
    function divider() {
        return h('panel', {
            size: [-100, 1], bg: T.C_BORDER, opa: 102, scrollable: false
        });
    }

    // ---- hitZone: 屏底 30px 透明上滑退出区（仿原生 home indicator）---------
    // opts: { id?, onExit }
    function hitZone(opts) {
        var o = opts || {};
        var accDy = 0;
        return h('panel', {
            id: o.id || '_hitZone',
            size: [-100, 30],
            align: ['bm', 0, 0],
            bg: 0x000000, bgOpa: 0,
            scrollable: false,
            onPress:   function () { accDy = 0; },
            onDrag:    function (e) { accDy += (e.dy | 0); },
            onRelease: function () {
                var up = -accDy;   // 上滑为正
                if (up >= 30 && o.onExit) o.onExit();
                accDy = 0;
            }
        });
    }

    // ---- swipeExit: 一行启用屏底上滑退出（hitZone 的别名+追加 root） -------
    // 用法: UI.swipeExit(rootChildren, function(){ ... })
    //   传入 rootChildren 数组（screen 的子节点），返回一个新数组，末尾追加 hitZone
    function swipeExit(children, onExit) {
        var arr = (children || []).slice();
        arr.push(hitZone({ onExit: onExit }));
        return arr;
    }

    // ---- modal: 弹模态卡片（C 端实现，JS 通过 VDOM.showModal 注册回调） -----
    // opts: { title, body, action0?, action1?, onResult? }
    //   onResult(idx): idx = 0/1（按钮）, -1（点遮罩/下滑取消）
    function modal(opts) {
        return VDOM.showModal(opts || {});
    }

    // ---- toast: 屏底浮一条 + 自动消失 --------------------------------------
    function toast(text, durMs) {
        sys.ui.toast('' + text, durMs | 0);
    }

    // ---- fadeIn: 给已 mount 的对象做淡入动画 -------------------------------
    function fadeIn(id, delayMs) {
        sys.ui.fadeIn(id, delayMs | 0);
    }

    return {
        T: T, I: I,
        SIZE_CONTENT: SIZE_CONTENT,
        screen: screen, title: title, card: card,
        kvRow: kvRow, listRow: listRow,
        iconBtn: iconBtn, pillBtn: pillBtn,
        badge: badge, statusBar: statusBar,
        divider: divider, hitZone: hitZone,
        modal: modal, toast: toast, fadeIn: fadeIn,
        swipeExit: swipeExit
    };
})();

// ============================================================================
// Router —— 多级页面栈（纯 JS 层，零 native）
//
// 心智模型：
//   每个页是一个 fn(props) → vnode 的"页构造器"。Router 维护一个页栈，
//   栈顶页 visible，下层全部 hidden。push/pop 改 hidden flag 切换显示。
//   滚动位置 / canvas 内容 / setInterval 都不会丢（不 destroy DOM）。
//
// API：
//   Router.define(name, builder)     注册页构造器；builder = fn(props) → vnode
//   Router.start(name, props?)        启动；调用一次。第一次 push + attachRootListener
//   Router.push(name, props?)         入栈（栈深 +1）；同名页递归 push 自动加后缀
//   Router.replace(name, props?)      替换栈顶（栈深不变）
//   Router.pop()                      出栈；栈深 1 时 = 上滑退出 app
//   Router.popTo(name)                出栈到指定 name；找不到 = sys.log warn
//   Router.current()                  → { name, props, depth }
//   Router.depth()                    → 栈深整数
//   Router.onEnter(name, fn)           可选生命周期：每次进入该页（push/pop 回退）触发
//   Router.onLeave(name, fn)           离开时触发（push 走 / pop 走 / replace 替换）
//
// 约束：
//   - builder 返回的 vnode 必须是 panel 类型（Router 把它当 page 容器）
//   - Router 自己往 vnode props 强制塞 size: [-100,-100] + 自动 id
//   - 不和 UI.modal / UI.toast 冲突（modal 走 sys.ui.modal，z-order 在所有 page 之上）
//   - 不接管 setInterval / ble.on，业务在 onLeave 里自行 clearInterval
// ============================================================================

var Router = (function () {
    var MAX_DEPTH = 4;

    var defs       = {};   // name → builder fn
    var enterCbs   = {};   // name → fn(props)
    var leaveCbs   = {};   // name → fn()
    var stack      = [];   // [{ name, slot, props }]，slot = 实际挂载用的 vnode id
    var counters   = {};   // name → 累计 push 次数（用于 id 唯一）
    var mounted    = {};   // slot id → true，避免重复 mount
    var started    = false;

    function autoSlot(name) {
        counters[name] = (counters[name] || 0) + 1;
        return name + '__r' + counters[name];
    }

    function mountPage(name, props, slot) {
        var builder = defs[name];
        if (!builder) {
            sys.log('Router: page not defined: ' + name);
            return false;
        }
        var node;
        try { node = builder(props || {}); }
        catch (eb) {
            sys.log('Router: builder(' + name + ') throw: ' + eb);
            return false;
        }
        if (!node || node.type !== 'panel') {
            sys.log('Router: builder(' + name + ') must return a panel vnode');
            return false;
        }
        // 强制 page 容器属性：撑满、覆盖整个可用区
        node.props.id = slot;
        node.props.size = [-100, -100];
        if (node.props.scrollable === undefined) node.props.scrollable = false;
        if (node.props.pad === undefined) node.props.pad = [0, 0, 0, 0];
        VDOM.mount(node, null);
        mounted[slot] = true;
        return true;
    }

    function setVisible(slot, visible) {
        if (mounted[slot]) {
            VDOM.set(slot, { hidden: !visible });
        }
    }

    function fireLeave(name) {
        var fn = leaveCbs[name];
        if (typeof fn === 'function') {
            try { fn(); } catch (eL) { sys.log('Router.onLeave(' + name + '): ' + eL); }
        }
    }

    function fireEnter(name, props) {
        var fn = enterCbs[name];
        if (typeof fn === 'function') {
            try { fn(props || {}); } catch (eE) { sys.log('Router.onEnter(' + name + '): ' + eE); }
        }
    }

    // 每个页栈 ≥2 时屏底上滑 = pop；=1 时让宿主层退出 app（不拦截）
    var swipeAccDy = 0;
    function ensureSwipeZone() {
        if (mounted['_routerSwipe']) return;
        var zone = h('panel', {
            id: '_routerSwipe',
            size: [-100, 30],
            align: ['bm', 0, 0],
            bg: 0x000000, bgOpa: 0,
            scrollable: false,
            onPress:   function () { swipeAccDy = 0; },
            onDrag:    function (e) { swipeAccDy += (e.dy | 0); },
            onRelease: function () {
                var up = -swipeAccDy;
                swipeAccDy = 0;
                if (up >= 30 && stack.length >= 2) {
                    pop();
                }
                /* 栈深 = 1 时不响应：事件穿透到宿主层 hit zone 退出 app */
            }
        });
        VDOM.mount(zone, null);
        mounted['_routerSwipe'] = true;
    }

    function start(name, props) {
        if (started) {
            sys.log('Router: already started');
            return;
        }
        started = true;
        var slot = autoSlot(name);
        if (!mountPage(name, props, slot)) { started = false; return; }
        stack.push({ name: name, slot: slot, props: props || {} });
        ensureSwipeZone();
        sys.ui.attachRootListener(slot);
        fireEnter(name, props);
    }

    function push(name, props) {
        if (!started) { sys.log('Router.push before start'); return; }
        if (stack.length >= MAX_DEPTH) {
            sys.log('Router: max depth ' + MAX_DEPTH + ' reached');
            return;
        }
        var top = stack[stack.length - 1];
        // 旧栈顶隐藏 + 触发 onLeave
        setVisible(top.slot, false);
        fireLeave(top.name);
        // 新页 mount + 显示
        var slot = autoSlot(name);
        if (!mountPage(name, props, slot)) {
            setVisible(top.slot, true);   // 回滚
            return;
        }
        stack.push({ name: name, slot: slot, props: props || {} });
        // 新页一定在栈顶，可见
        setVisible(slot, true);
        // 新页也要 attachRootListener，否则页内按钮事件无法分发
        sys.ui.attachRootListener(slot);
        fireEnter(name, props);
    }

    function pop() {
        if (!started) { sys.log('Router.pop before start'); return; }
        if (stack.length <= 1) {
            sys.log('Router.pop: at root');
            return;
        }
        var leaving = stack.pop();
        fireLeave(leaving.name);
        setVisible(leaving.slot, false);
        // 销毁该页（pop 后没必要保留）
        VDOM.destroy(leaving.slot);
        delete mounted[leaving.slot];
        var top = stack[stack.length - 1];
        setVisible(top.slot, true);
        fireEnter(top.name, top.props);
    }

    function replace(name, props) {
        if (!started) { sys.log('Router.replace before start'); return; }
        if (stack.length === 0) {
            start(name, props); return;
        }
        var leaving = stack.pop();
        fireLeave(leaving.name);
        VDOM.destroy(leaving.slot);
        delete mounted[leaving.slot];
        var slot = autoSlot(name);
        if (!mountPage(name, props, slot)) {
            sys.log('Router.replace: mount failed, stack now empty-ish');
            return;
        }
        stack.push({ name: name, slot: slot, props: props || {} });
        setVisible(slot, true);
        sys.ui.attachRootListener(slot);
        fireEnter(name, props);
    }

    function popTo(name) {
        if (!started) { sys.log('Router.popTo before start'); return; }
        var idx = -1;
        for (var i = stack.length - 1; i >= 0; i--) {
            if (stack[i].name === name) { idx = i; break; }
        }
        if (idx < 0) { sys.log('Router.popTo: ' + name + ' not in stack'); return; }
        if (idx === stack.length - 1) return;
        // 逐个 pop 直到栈顶就是 name
        while (stack.length - 1 > idx) {
            pop();
        }
    }

    function current() {
        if (stack.length === 0) return null;
        var top = stack[stack.length - 1];
        return { name: top.name, props: top.props, depth: stack.length };
    }

    function depth() { return stack.length; }

    function onEnter(name, fn) { enterCbs[name] = fn; }
    function onLeave(name, fn) { leaveCbs[name] = fn; }

    return {
        define: function (n, b) { defs[n] = b; },
        start:   start,
        push:    push,
        replace: replace,
        pop:     pop,
        popTo:   popTo,
        current: current,
        depth:   depth,
        onEnter: onEnter,
        onLeave: onLeave
    };
})();

// ============================================================================
// setTimeout / clearTimeout —— 基于 setInterval 包一层，提供一次性定时器。
//
// 平台 native 只暴露 setInterval / clearInterval。但业务很常见"N 毫秒后做一次
// X" 的需求，硬要用 setInterval + 自管 clearInterval 既丑又容易漏清。
//
// 行为：
//   setTimeout(fn, ms)     ms 后调一次 fn 然后自动清理；返回 timer id
//   clearTimeout(id)       提前取消（id 必须是 setTimeout 返回值）
//
// 边界：
//   - 不支持 fn 抛异常时的传播，会 sys.log warn
//   - id 跟 setInterval 共用 ID 池：clearInterval(timeoutId) 也能取消（不推荐）
// ============================================================================

function setTimeout(fn, ms) {
    var fired = false;
    var id = setInterval(function () {
        if (fired) return;          // 极端情况下 timer 在被 clear 之前重入：忽略
        fired = true;
        clearInterval(id);
        try { fn(); } catch (e) { sys.log('setTimeout cb: ' + e); }
    }, ms | 0);
    return id;
}

function clearTimeout(id) {
    clearInterval(id);
}

