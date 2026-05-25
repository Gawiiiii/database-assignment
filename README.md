# Database Assignment

本仓库整理数据库系统作业提交材料。

## Assignment 2

第二次作业位于：

`assignment2-db-storage-project/`

内容包括：

- C++17 简化记录存储引擎源码；
- schema parser；
- 变长记录序列化与反序列化；
- 4KB Page 和 Slot Directory；
- 插入、删除、更新、RID 读取；
- 内存 B+ 树主键索引；
- synthetic 和 TPC-H orders loader；
- benchmark 脚本和图表绘制脚本。

为避免提交超过 GitHub 单文件限制的大型 TPC-H 数据文件，仓库只包含 `data/orders.tbl` 小样本。百万级实验可使用 synthetic 数据直接复现，或在本地用 TPC-H dbgen 生成完整 `orders.tbl` 后运行。
