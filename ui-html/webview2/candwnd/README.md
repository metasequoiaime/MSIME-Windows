# Candidate window templates

候选窗口的 DOM、测量脚本和交互脚本只维护两份：

- `horizontal_candidate_window.html`
- `vertical_candidate_window.html`

内置皮肤采用“一皮肤一文件夹”的结构：

```text
skins/
├─ fluent/
├─ wechat/
├─ graphite/
├─ willow_green/
└─ autumn_osmanthus/
```

每个皮肤目录包含 `horizontal_dark.css`、`horizontal_light.css`、`vertical_dark.css` 和 `vertical_light.css`。Server 根据基础皮肤、候选框布局和明暗模式组合资源路径，并以内联 `<style>` 注入共享 HTML；外部皮肤的 CSS 随后加载，用于覆盖基础皮肤。

`ApplyCandidateFrame` 只隐藏用不到的候选行、不删除它们，所以 `:last-child` 不一定是最后一个可见行；需要定位尾项的皮肤用它维护的 `.row-wrapper.last-visible`。

外部皮肤使用 `skin.toml` 声明几何与候选配色，可选 `toolbar.css` 覆盖悬浮工具栏。不提供候选框 HTML 或 `cand.css`。
