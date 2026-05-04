#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 菜单页通用 modal —— 当前只用于动态 app 的"删除确认"。
 *
 * 设计：
 *   - 挂在 lv_layer_top()，覆盖所有 screen，不依赖任何特定页
 *   - 半透明遮罩拦截背景点击，必须点 Cancel/Delete 才能关
 *   - 一次性：show 创建，按钮点完销毁，无全局状态
 *
 * 线程：必须在 UI 线程调用。
 */

/**
 * 弹出"删除 <app_name>?"确认对话框。
 *
 * @param app_name    用于显示标题。生命周期仅需到本函数返回（内部会拷贝）。
 * @param on_confirm  用户点 Delete 时回调；点 Cancel 时不调。可为 NULL。
 * @param user_data   原样传给 on_confirm。
 *
 * 行为：
 *   - 已有 modal 在屏 → 先销毁旧的再开新的
 *   - 不阻塞，立即返回
 */
void launcher_modal_show_delete_confirm(const char *app_name,
                                        void (*on_confirm)(void *ud),
                                        void *user_data);

/* 销毁当前 modal（如果存在）—— launcher 退出 app 时调用，避免 layer_top 残留 */
void launcher_modal_dismiss(void);

#ifdef __cplusplus
}
#endif
