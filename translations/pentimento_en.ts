<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="zh_CN">
<context>
    <name>Library</name>
    <message>
        <location filename="../src/library.cpp" line="+27"/>
        <location line="+19"/>
        <location line="+6"/>
        <location line="+7"/>
        <location line="+6"/>
        <source>无法读取文件：%1</source>
        <translation>Cannot read file: %1</translation>
    </message>
    <message>
        <location line="-36"/>
        <source>首版单文件上限为 256 MiB：%1</source>
        <translation>First release caps single files at 256 MiB: %1</translation>
    </message>
    <message>
        <location line="+8"/>
        <source>无法安全写入文件：%1；%2</source>
        <translation>Cannot safely write file: %1; %2</translation>
    </message>
    <message>
        <source>无法创建素材库目录</source>
        <translation type="vanished">Cannot create library directories</translation>
    </message>
    <message>
        <location line="+48"/>
        <source>该目录不是有效的素材库（未找到 catalog.sqlite），不会静默重建</source>
        <translation>Not a valid asset library (catalog.sqlite missing); will not silently recreate it</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>素材库索引不可读（权限不足），请检查目录权限</source>
        <translation>Library index is unreadable (permission denied); check directory permissions</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>素材库目录不可达或无法创建（可能是卷未挂载或权限不足）：%1</source>
        <translation>Library directory is unreachable or cannot be created (volume not mounted or permission denied): %1</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>素材库正在被另一个进程使用</source>
        <translation>The library is in use by another process</translation>
    </message>
    <message>
        <location line="+24"/>
        <source>素材库损坏或索引无法打开：%1</source>
        <translation>Library is corrupt or the index cannot be opened: %1</translation>
    </message>
    <message>
        <location line="+79"/>
        <source>素材库 schema 版本不兼容（%1），请使用匹配版本打开</source>
        <translation>Library schema version is incompatible (%1); open with a matching version</translation>
    </message>
    <message>
        <location line="+29"/>
        <source>素材库缺少身份标识（library_id），库可能已损坏</source>
        <translation>Library is missing its identity (library_id); the library may be corrupt</translation>
    </message>
    <message>
        <location line="+139"/>
        <source>无效的评分：%1</source>
        <translation>Invalid rating: %1</translation>
    </message>
    <message>
        <location line="+120"/>
        <source>排除规则不能为空</source>
        <translation>Exclusion rule must not be empty</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>未知的排除规则类型</source>
        <translation>Unknown exclusion rule kind</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>该排除规则已存在</source>
        <translation>This exclusion rule already exists</translation>
    </message>
    <message>
        <location line="+40"/>
        <source>版本不存在</source>
        <translation>Version not found</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>手动固定</source>
        <translation>Pinned manually</translation>
    </message>
    <message>
        <location line="+23"/>
        <source>名称不能为空</source>
        <translation>Name cannot be empty</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>名称不能包含路径分隔符</source>
        <translation>Name cannot contain a path separator</translation>
    </message>
    <message>
        <location line="+5"/>
        <location line="+51"/>
        <source>找不到素材</source>
        <translation>Asset not found</translation>
    </message>
    <message>
        <location line="-41"/>
        <source>同文件夹已有同名素材：%1，不会覆盖</source>
        <translation>An asset with this name already exists in this folder: %1; will not overwrite</translation>
    </message>
    <message>
        <location line="+12"/>
        <source>无法重命名工作副本</source>
        <translation>Cannot rename the working copy</translation>
    </message>
    <message>
        <location line="+15"/>
        <source>文件夹必须是库内相对路径</source>
        <translation>The folder must be a library-relative path</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>文件夹不能超出素材库</source>
        <translation>The folder cannot escape the library</translation>
    </message>
    <message>
        <location line="+18"/>
        <source>目标文件夹已有同名素材：%1，不会合并</source>
        <translation>An asset with this name already exists in the target folder: %1; will not merge</translation>
    </message>
    <message>
        <location line="+35"/>
        <source>该路径不是有效的素材库（未找到可读的 catalog.sqlite）</source>
        <translation>This path is not a valid asset library (no readable catalog.sqlite found)</translation>
    </message>
    <message>
        <location line="+16"/>
        <source>该素材库缺少身份标识（library_id），库可能已损坏</source>
        <translation>This asset library is missing its identity (library_id); the library may be corrupt</translation>
    </message>
    <message>
        <location line="+36"/>
        <location line="+2"/>
        <source>无效的对象哈希</source>
        <translation>Invalid object hash</translation>
    </message>
    <message>
        <location line="+7"/>
        <location line="+13"/>
        <location line="+18"/>
        <location line="+19"/>
        <location line="+3"/>
        <source>历史对象校验失败；已停止操作</source>
        <translation>History object verification failed; operation stopped</translation>
    </message>
    <message>
        <location line="-314"/>
        <source>历史对象内容不一致</source>
        <translation>History object content mismatch</translation>
    </message>
    <message>
        <location line="-400"/>
        <location line="+7"/>
        <location line="+4"/>
        <source>无法写入暂存对象：%1</source>
        <translation>Cannot write the staged object: %1</translation>
    </message>
    <message>
        <location line="+706"/>
        <source>无法发布历史对象</source>
        <translation>Cannot publish the history object</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>无法创建对象暂存目录</source>
        <translation>Cannot create the object staging directory</translation>
    </message>
    <message>
        <location line="+43"/>
        <source>无法创建工作目录</source>
        <translation>Cannot create working directory</translation>
    </message>
    <message>
        <location line="+87"/>
        <source>无法创建恢复目录</source>
        <translation>Cannot create restore directory</translation>
    </message>
    <message>
        <location line="+12"/>
        <source>找不到属于当前素材的历史版本</source>
        <translation>No revision found for this asset</translation>
    </message>
    <message>
        <location line="-457"/>
        <location line="+467"/>
        <source>请导出到素材库之外的现有目录</source>
        <translation>Export to an existing directory outside the library</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>找不到历史版本</source>
        <translation>Revision not found</translation>
    </message>
</context>
<context>
    <name>MainWindow</name>
    <message>
        <location filename="../src/mainwindow.cpp" line="+37"/>
        <source>pentimento · 画迹</source>
        <translation>pentimento</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>素材操作</source>
        <translation>Asset actions</translation>
    </message>
    <message>
        <location line="+5"/>
        <source>导入素材</source>
        <translation>Import assets</translation>
    </message>
    <message>
        <location line="+2"/>
        <location line="+886"/>
        <source>记录版本</source>
        <translation>Record version</translation>
    </message>
    <message>
        <location line="-885"/>
        <source>编辑工作副本</source>
        <translation>Edit working copy</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>无法打开默认编辑器；可通过素材库目录打开工作副本</source>
        <translation>Cannot open the default editor; open the working copy from the library folder</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>打开素材库</source>
        <translation>Open library folder</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>网格视图</source>
        <translation>Grid view</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>列表视图</source>
        <translation>List view</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>为筛选结果添加标签…</source>
        <translation>Add tag to filtered results…</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>从筛选结果移除标签…</source>
        <translation>Remove tag from filtered results…</translation>
    </message>
    <message>
        <location line="+2"/>
        <location line="+1127"/>
        <location line="+6"/>
        <source>恢复到锚点</source>
        <translation>Restore to anchor</translation>
    </message>
    <message>
        <location line="-1129"/>
        <source>素材库</source>
        <translation>Library</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>搜索素材名称</source>
        <translation>Search asset names</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>搜索素材</source>
        <translation>Search assets</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>素材列表</source>
        <translation>Asset list</translation>
    </message>
    <message>
        <location line="+51"/>
        <location line="+684"/>
        <location line="+16"/>
        <source>标签</source>
        <translation>Tags</translation>
    </message>
    <message>
        <location line="-697"/>
        <source>标签列表</source>
        <translation>Tag list</translation>
    </message>
    <message>
        <location line="+5"/>
        <source>新标签</source>
        <translation>New tag</translation>
    </message>
    <message>
        <location line="+1"/>
        <location line="+978"/>
        <source>添加</source>
        <translation>Add</translation>
    </message>
    <message>
        <location line="-977"/>
        <source>移除</source>
        <translation>Remove</translation>
    </message>
    <message>
        <location line="+10"/>
        <location line="+3"/>
        <source>评分</source>
        <translation>Rating</translation>
    </message>
    <message>
        <location line="-40"/>
        <location line="+41"/>
        <source>无评分</source>
        <translation>No rating</translation>
    </message>
    <message>
        <location line="-66"/>
        <source>筛选</source>
        <translation>Filter</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>标签（逗号分隔，任一命中）</source>
        <translation>Tags (comma-separated, any match)</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>格式筛选</source>
        <translation>Format filter</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>全部格式</source>
        <translation>All formats</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>最小宽度</source>
        <translation>Minimum width</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>宽度不限</source>
        <translation>Any width</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>最小高度</source>
        <translation>Minimum height</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>高度不限</source>
        <translation>Any height</translation>
    </message>
    <message>
        <location line="+5"/>
        <source>评分筛选</source>
        <translation>Rating filter</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>全部评分</source>
        <translation>All ratings</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>≥ %1 星</source>
        <translation>≥ %1 star(s)</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>清除条件</source>
        <translation>Clear conditions</translation>
    </message>
    <message>
        <location line="+40"/>
        <source>%1 星</source>
        <translation>%1 star(s)</translation>
    </message>
    <message>
        <location line="+1"/>
        <location line="+3"/>
        <source>备注</source>
        <translation>Note</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>备注将在失焦或点击保存时写入</source>
        <translation>The note is saved on focus loss or when you click Save</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>保存备注</source>
        <translation>Save note</translation>
    </message>
    <message>
        <location line="+11"/>
        <source>导入第一份素材，开始保留创作历史。</source>
        <translation>Import the first asset to start keeping creative history.</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>适应窗口</source>
        <translation>Fit window</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>100%</source>
        <translation>100%</translation>
    </message>
    <message>
        <location line="+7"/>
        <source>透明底</source>
        <translation>Transparent</translation>
    </message>
    <message>
        <location line="+17"/>
        <source>素材预览

支持 PNG、JPEG、WebP 等已安装的 Qt 图像格式
其他文件保留原始内容，暂不提供画面预览</source>
        <translation>Asset preview

Supports Qt image formats installed on this system (PNG, JPEG, WebP...)
Other files keep their original bytes; no picture preview yet</translation>
    </message>
    <message>
        <location line="+31"/>
        <source>版本时间轴</source>
        <translation>Version timeline</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>选择历史版本预览
恢复会新增版本，原有历史始终保留。</source>
        <translation>Select a revision to preview
Restoring appends a new version; existing history is always kept.</translation>
    </message>
    <message>
        <location line="+3"/>
        <location line="+619"/>
        <source>历史版本</source>
        <translation>Historical version</translation>
    </message>
    <message>
        <location line="-616"/>
        <source>恢复此版本</source>
        <translation>Restore this version</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>导出此版本…</source>
        <translation>Export this version...</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>来源与授权</source>
        <translation>Source &amp; license</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>来源（网址或出处，可留空）</source>
        <translation>Source (URL or origin, can be empty)</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>作者（可留空）</source>
        <translation>Author (can be empty)</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>许可（如实填写；应用不代为判断）</source>
        <translation>License (enter truthfully; the app does not decide it for you)</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>附加授权证据…</source>
        <translation>Attach license evidence…</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>记录</source>
        <translation>Record</translation>
    </message>
    <message>
        <location line="+1"/>
        <location line="+408"/>
        <source>未附加证据</source>
        <translation>No evidence attached</translation>
    </message>
    <message>
        <location line="-403"/>
        <source>记录（旧记录可回看）</source>
        <translation>Records (old entries stay viewable)</translation>
    </message>
    <message>
        <location line="+5"/>
        <source>导出证据…</source>
        <translation>Export evidence…</translation>
    </message>
    <message>
        <location line="+16"/>
        <source>定时快照</source>
        <translation>Scheduled snapshots</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>关闭</source>
        <translation>Off</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>每 15 分钟</source>
        <translation>Every 15 minutes</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>每 30 分钟</source>
        <translation>Every 30 minutes</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>每 60 分钟</source>
        <translation>Every 60 minutes</translation>
    </message>
    <message>
        <location line="+1"/>
        <location line="+739"/>
        <source>定时快照未启用</source>
        <translation>Scheduled snapshots off</translation>
    </message>
    <message>
        <location line="-736"/>
        <location line="+105"/>
        <location line="+606"/>
        <location line="+11"/>
        <source>暂停</source>
        <translation>Pause</translation>
    </message>
    <message>
        <location line="-720"/>
        <source>仅应用运行时生效；退出应用期间不算持续备份。</source>
        <translation>Active only while the app runs; exiting is never counted as continuous backup.</translation>
    </message>
    <message>
        <location line="+5"/>
        <source>排除规则…</source>
        <translation>Exclusion rules…</translation>
    </message>
    <message>
        <location line="+24"/>
        <source>重命名…</source>
        <translation>Rename…</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>移动到文件夹…</source>
        <translation>Move to folder…</translation>
    </message>
    <message>
        <location line="+14"/>
        <source>取消固定</source>
        <translation>Unpin</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>已取消固定版本 #%1（历史保留）</source>
        <translation>Unpinned version #%1 (history kept)</translation>
    </message>
    <message>
        <location line="+5"/>
        <source>固定此版本…</source>
        <translation>Pin this version…</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>固定关键版本</source>
        <translation>Pin key version</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>固定原因（固定版本不会被保留策略清理）</source>
        <translation>Pin reason (a pinned version is never cleaned by the retention policy)</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>已固定版本 #%1</source>
        <translation>Pinned version #%1</translation>
    </message>
    <message>
        <location line="+38"/>
        <source>系统（应用退到后台）</source>
        <translation>System (app in background)</translation>
    </message>
    <message>
        <location line="+1"/>
        <location line="+629"/>
        <source>恢复</source>
        <translation>Resume</translation>
    </message>
    <message>
        <location line="-605"/>
        <source>本地素材库：%1 · 编辑工作副本后自动记录稳定变化</source>
        <translation>Local library: %1 · stable changes to working copies are recorded automatically</translation>
    </message>
    <message>
        <location line="+25"/>
        <source>名称:%1</source>
        <translation>Name: %1</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>标签:%1</source>
        <translation>Tags: %1</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>格式:%1</source>
        <translation>Format: %1</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>尺寸:≥%1×%2</source>
        <translation>Size: ≥%1×%2</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>评分:无评分</source>
        <translation>Rating: no rating</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>评分:≥%1 星</source>
        <translation>Rating: ≥%1 star(s)</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>无生效条件</source>
        <translation>No active conditions</translation>
    </message>
    <message>
        <location line="+34"/>
        <source>操作未完成</source>
        <translation>Operation not completed</translation>
    </message>
    <message>
        <location line="+110"/>
        <source>当前</source>
        <translation>Current</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>已固定</source>
        <translation>Pinned</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>固定原因：%1</source>
        <translation>Pin reason: %1</translation>
    </message>
    <message>
        <source>当前 · </source>
        <translation type="vanished">Current · </translation>
    </message>
    <message>
        <location line="+54"/>
        <source>已清除评分</source>
        <translation>Rating cleared</translation>
    </message>
    <message>
        <location line="+0"/>
        <source>已保存评分：%1 星</source>
        <translation>Rating saved: %1 star(s)</translation>
    </message>
    <message>
        <location line="+18"/>
        <source>作者：%1</source>
        <translation>Author: %1</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>许可：%1</source>
        <translation>License: %1</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>证据：%1</source>
        <translation>Evidence: %1</translation>
    </message>
    <message>
        <location line="+8"/>
        <source>选择授权证据文件</source>
        <translation>Choose evidence file</translation>
    </message>
    <message>
        <location line="+5"/>
        <source>待记录证据：%1</source>
        <translation>Evidence to record: %1</translation>
    </message>
    <message>
        <location line="+19"/>
        <source>已记录来源与授权</source>
        <translation>Source &amp; license recorded</translation>
    </message>
    <message>
        <location line="+9"/>
        <source>重命名</source>
        <translation>Rename</translation>
    </message>
    <message>
        <location line="+0"/>
        <source>新名称</source>
        <translation>New name</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>已重命名：%1</source>
        <translation>Renamed: %1</translation>
    </message>
    <message>
        <location line="+9"/>
        <source>移动到文件夹</source>
        <translation>Move to folder</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>文件夹（库内相对路径，空为根目录）</source>
        <translation>Folder (library-relative path, empty = root)</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>已移到根目录</source>
        <translation>Moved to root</translation>
    </message>
    <message>
        <location line="+0"/>
        <source>已移动到：%1</source>
        <translation>Moved to: %1</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>导出授权证据</source>
        <translation>Export license evidence</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>授权证据已导出：%1</source>
        <translation>License evidence exported: %1</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>备注已保存</source>
        <translation>Note saved</translation>
    </message>
    <message>
        <location line="+16"/>
        <source>标签不能为空</source>
        <translation>Tag cannot be empty</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>已添加标签：%1</source>
        <translation>Added tag: %1</translation>
    </message>
    <message>
        <location line="+0"/>
        <source>标签已存在：%1</source>
        <translation>Tag already exists: %1</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>请先选择一个标签</source>
        <translation>Select a tag first</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>已移除标签：%1</source>
        <translation>Removed tag: %1</translation>
    </message>
    <message>
        <location line="+7"/>
        <source>为筛选结果添加标签</source>
        <translation>Add tag to filtered results</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>筛选结果中的素材都已带此标签</source>
        <translation>All assets in the filtered results already have this tag</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>批量添加标签</source>
        <translation>Batch add tag</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>将给 %1 个素材添加标签「%2」（已有该标签的不变）。
不会覆盖其他标签。</source>
        <translation>Will add tag &quot;%2&quot; to %1 assets (those already having it are unchanged).
Other tags are not affected.</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>已为 %1 个素材添加标签：%2</source>
        <translation>Added tag %2 to %1 assets</translation>
    </message>
    <message>
        <location line="+7"/>
        <source>从筛选结果移除标签</source>
        <translation>Remove tag from filtered results</translation>
    </message>
    <message>
        <location line="+4"/>
        <source>筛选结果中没有素材带此标签</source>
        <translation>No asset in the filtered results has this tag</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>批量移除标签</source>
        <translation>Batch remove tag</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>将从 %1 个素材移除标签「%2」。
只移除这一个标签，不影响其他标签。</source>
        <translation>Will remove tag &quot;%2&quot; from %1 assets.
Only this tag is removed; other tags are not affected.</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>已从 %1 个素材移除标签：%2</source>
        <translation>Removed tag %2 from %1 assets</translation>
    </message>
    <message>
        <location line="+20"/>
        <source>%1 MiB</source>
        <translation>%1 MiB</translation>
    </message>
    <message>
        <location line="+8"/>
        <source>当前版本</source>
        <translation>Current version</translation>
    </message>
    <message>
        <location line="+8"/>
        <source>内容损坏或校验失败

%1

原始对象已保留，可导出排查。</source>
        <translation>Content corrupted or verification failed

%1

The original object is preserved; export it for inspection.</translation>
    </message>
    <message>
        <location line="+15"/>
        <source>原文件已保留，暂无合成预览（PSD 解析未支持）

Photoshop 合成图解析不在当前版本范围内（Qt 6.10.2 无 PSD 解码插件）。
原始文件和历史版本已完整保留，可以导出查看。</source>
        <translation>Original file is kept; no composite preview yet (PSD parsing unsupported)

Photoshop composite parsing is out of scope for this version (Qt 6.10.2 has no PSD decode plugin).
The original file and history versions are fully preserved and can be exported.</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>此格式暂不支持画面预览

原始文件和历史版本已完整保留，可以导出查看。</source>
        <translation>No picture preview for this format yet

The original file and history are fully preserved and can be exported.</translation>
    </message>
    <message>
        <location line="+9"/>
        <source>预览已降采样显示（解码尺寸超出内存预算），原始文件未修改</source>
        <translation>Preview downsampled for display (decode size exceeded memory budget); source file unchanged</translation>
    </message>
    <message>
        <location line="+13"/>
        <source>导入素材（保留原文件，创建工作副本）</source>
        <translation>Import assets (originals kept, working copies created)</translation>
    </message>
    <message>
        <location line="+15"/>
        <source>版本说明</source>
        <translation>Version note</translation>
    </message>
    <message>
        <location line="+8"/>
        <source>已记录新版本 #%1</source>
        <translation>Recorded new version #%1</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>内容未变化（当前版本 #%1），无需重复保存</source>
        <translation>Content unchanged (current version #%1); nothing to save again</translation>
    </message>
    <message>
        <location line="+251"/>
        <source>（%1 个被排除）</source>
        <translation>(%1 excluded)</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>#%1 %2 · %3 个素材%4</source>
        <translation>#%1 %2 · %3 assets%4</translation>
    </message>
    <message>
        <source>已记录新版本</source>
        <translation type="vanished">New version recorded</translation>
    </message>
    <message>
        <source>内容未变化，无需重复保存</source>
        <translation type="vanished">Content unchanged; nothing to save</translation>
    </message>
    <message>
        <location line="-244"/>
        <source>恢复历史版本</source>
        <translation>Restore revision</translation>
    </message>
    <message>
        <location line="+0"/>
        <source>将历史内容恢复为当前工作副本，并新增一个版本。
未记录的当前内容会先保存。请先在外部编辑器中关闭此文件。</source>
        <translation>Restores the historical content as the current working copy and appends a new version.
Unrecorded current content is saved first. Close the file in external editors.</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>导出历史版本</source>
        <translation>Export revision</translation>
    </message>
    <message>
        <location line="+24"/>
        <source>自动记录失败：%1</source>
        <translation>Auto-record failed: %1</translation>
    </message>
    <message>
        <location line="+54"/>
        <source>暂停定时快照</source>
        <translation>Pause scheduled snapshots</translation>
    </message>
    <message>
        <location line="+0"/>
        <source>暂停原因</source>
        <translation>Pause reason</translation>
    </message>
    <message>
        <location line="+1"/>
        <location line="+2"/>
        <source>用户暂停</source>
        <translation>Paused by user</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>已暂停：%1</source>
        <translation>Paused: %1</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>快照进行中…</source>
        <translation>Snapshot in progress…</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>下次快照：%1</source>
        <translation>Next snapshot: %1</translation>
    </message>
    <message>
        <location line="+46"/>
        <source>定时快照失败：%1</source>
        <translation>Scheduled snapshot failed: %1</translation>
    </message>
    <message>
        <location line="+9"/>
        <source>版本排除规则</source>
        <translation>Version exclusion rules</translation>
    </message>
    <message>
        <location line="+3"/>
        <source>命中规则的对象不进入后续快照；历史保留，被排除 ≠ 已删除，恢复时不会触碰其当前文件。</source>
        <translation>Assets matching a rule stay out of later snapshots; history is kept, excluded is not deleted, and a restore never touches their current file.</translation>
    </message>
    <message>
        <location line="+4"/>
        <location line="+16"/>
        <source>扩展名</source>
        <translation>Extension</translation>
    </message>
    <message>
        <location line="-15"/>
        <location line="+16"/>
        <source>路径</source>
        <translation>Path</translation>
    </message>
    <message>
        <location line="-15"/>
        <location line="+15"/>
        <source>名称</source>
        <translation>Name</translation>
    </message>
    <message>
        <location line="-13"/>
        <source>如：psd、scenes、hero_*.png</source>
        <translation>e.g. psd, scenes, hero_*.png</translation>
    </message>
    <message>
        <location line="+9"/>
        <source>删除选中</source>
        <translation>Remove selected</translation>
    </message>
    <message>
        <location line="+22"/>
        <source>无法添加排除规则</source>
        <translation>Cannot add exclusion rule</translation>
    </message>
    <message>
        <location line="+10"/>
        <source>无法删除排除规则</source>
        <translation>Cannot remove exclusion rule</translation>
    </message>
    <message>
        <location line="+15"/>
        <source>紧急快照失败：%1</source>
        <translation>Emergency snapshot failed: %1</translation>
    </message>
    <message>
        <location line="+2"/>
        <source>疑似勒索批量写入</source>
        <translation>Suspected ransomware batch write</translation>
    </message>
    <message>
        <location line="+1"/>
        <source>工作区检测到异常批量写入，已自动建立紧急快照锚点。

%1

可先关闭外部编辑器，再通过「恢复历史版本」或「恢复到锚点」回退。</source>
        <translation>Abnormal batch writes detected in the workspace; an emergency snapshot anchor was taken automatically.

%1

Close external editors, then roll back via &quot;Restore revision&quot; or &quot;Restore to anchor&quot;.</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>还没有紧急快照锚点</source>
        <translation>No emergency snapshot anchor yet</translation>
    </message>
    <message>
        <source>#%1 %2 · %3 个素材</source>
        <translation type="vanished">#%1 %2 · %3 assets</translation>
    </message>
    <message>
        <location line="+16"/>
        <source>选择感染前锚点（回滚形成新版本，不销毁后续历史）</source>
        <translation>Choose a pre-infection anchor (rollback forms new versions; later history is preserved)</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>将整个工作区恢复到该锚点。
未记录的当前内容会先保存，后续历史（含被加密版本）保留。
请先关闭所有外部编辑器。</source>
        <translation>Restores the whole workspace to this anchor.
Unrecorded current content is saved first; later history (including encrypted versions) is kept.
Close all external editors first.</translation>
    </message>
    <message>
        <location line="+6"/>
        <source>已恢复到锚点 #%1（%2 个素材回滚）</source>
        <translation>Restored to anchor #%1 (%2 assets rolled back)</translation>
    </message>
</context>
<context>
    <name>QObject</name>
    <message>
        <location filename="../src/importer.cpp" line="+50"/>
        <source>忽略规则 %1</source>
        <translation>Ignore rule %1</translation>
    </message>
    <message>
        <location line="+9"/>
        <source>根目录不可读：%1</source>
        <translation>Root directory is unreadable: %1</translation>
    </message>
    <message>
        <location line="+14"/>
        <source>不可读（跳过，可重试）：%1</source>
        <translation>Unreadable (skipped, can retry): %1</translation>
    </message>
</context>
</TS>
