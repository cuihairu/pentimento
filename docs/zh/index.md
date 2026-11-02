---
layout: home

hero:
  name: pentimento · 画迹
  text: 给游戏美术素材的版本历史
  tagline: 面向游戏美术素材的本地桌面资源管理器，以历史版本浏览与恢复为核心。C++17、Qt 6 Widgets、SQLite，不使用 WebView。
  actions:
    - theme: brand
      text: 快速开始
      link: /zh/quickstart
    - theme: alt
      text: 架构
      link: /zh/architecture

features:
  - icon: 🗂️
    title: 复制导入，不动原文件
    details: 创建本地素材库，批量导入以管理式复制进行；导入前的原始文件不会被修改。
  - icon: 🕰️
    title: 自动记录版本
    details: 应用运行时每两秒检查工作副本，连续两次大小/修改时间稳定后记录变化；相同内容不重复生成版本。
  - icon: ⏪
    title: 恢复只增不毁
    details: 浏览、导出历史版本。恢复时先保存尚未记录的当前内容，再新增版本——原有历史全部保留。
  - icon: 🔒
    title: 防勒索病毒防线
    details: append-only 只读对象库、在感染扩散前自动快照锚点的批量修改哨兵，以及一键整树回滚到最近干净锚点。
  - icon: 🧬
    title: SHA-256 内容寻址
    details: 相同文件内容复用对象；SQLite 保存索引，QSaveFile 原子写入，库级锁限制单进程访问。
  - icon: 🧪
    title: Qt Test 覆盖
    details: 存储与恢复行为由测试覆盖，经 CTest 运行；CMake 3.24 preset 是唯一构建入口。
