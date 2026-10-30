# 仓库事实修正记录（代决执行，2026-10-30）

> 状态：**已代决**。本文件记录执行立即动作时发现的分析结论与仓库事实冲突、以及仓库自身事实性错误的修正。规则：**分析结论与仓库事实冲突时以仓库为准**；仓库自身算术/计数错误按实际核实值修正。

## C-01 性能样本合计算术（performance.md:18）

- **冲突**：`docs/zh/performance.md:18` 称「合计：2,090 个文件、约 30 GiB」；按同文件表内单文件体积逐行重算为 **2,590 个文件、11,960 MiB ≈ 11.68 GiB**（见 00-scope-and-budgets#P-004 样本矩阵）。
- **处理**：回写 `performance.md:18` 为修正值（仓库事实=表内算术）；D-20 决策点随之按两档（约 5 GiB 快速档 + 约 11.68 GiB 完整档）调整口径，不以「30 GiB」为采购/下载要求。
- **待用户**：样本可用性/使用权限、三系统设备（见 00-scope-and-budgets#P-004）。

## C-02 todo.en.md 缺「已代决」标注（P-608 核对项 1/2）

- **冲突**：todo.md 93 项 BLOCKED 均有「已代决:按 docs/review/2026-10-30-decisions.md 执行(2026-10-30)」；todo.en.md 无此标注、未引用 decisions 文档。
- **处理**：todo.en.md 93 项同步补相同标注，并加 decisions 文档引用；与 todo.md 逐项一致。

## C-03 「94 项」计数口径（P-608 核对项 3）

- **冲突**：todo.md 审核历史称「todo.en.md 全部 94 项未完成任务已补 BLOCKED 标注」；经核对实际未完成 BLOCKED 为 **93 项**（P-005、P-101 已完成，不纳入；与 decisions 文档计数一致）。
- **处理**：该条为历史审核记录，**不篡改历史勾选**；在本文件注明正确口径 93，避免与 decisions 文档冲突。

## C-04 Qt 图像格式支持事实（30-foundation#P-102）

- **事实**：本机 Qt 6.10.2 `QImageReader::supportedImageFormats()` = bmp, cur, gif, ico, jfif, jpeg, jpg, pbm, pgm, png, ppm, svg, svgz, xbm, xpm。
- **影响**：V1 五格式中 **WebP/TGA 本机不可预览**（无 `libqwebp`/`libqtga` 插件）；PNG/JPEG/BMP 可预览。按「降级：暂无预览，原字节保留」处理，或补充插件；不作「已支持」结论（见 40-library#P-210 样本表）。
