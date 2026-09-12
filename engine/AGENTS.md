# AGENTS.md — engine/

本目录是本仓库的输入引擎组件，不再是子模块。来源提交和专门化时裁掉的内容见 [UPSTREAM.md](UPSTREAM.md)；仓库级约定以根 `AGENTS.md` 为准，本文件补充引擎自身的实现、数据和验证规则。

**这里是一等代码，不是 vendored 第三方。** 需要为 Windows 改引擎就直接改，和改 `server/` 一样评审和测试，不要为了"保持和上游一致"而绕路。唯一例外是 `googlepinyinime-rev/`、`utfcpp/` 和 `voice/third_party/`：那三个确实是上游副本，保留其格式与许可，别去重排版。

`helpcode/` 负责辅助码。`voice/` 是可选语音模块，规则见 `voice/AGENTS.md`；导入的 Windows 独立程序位于 `voice/platforms/windows/`，默认不构建，也不进入公共库目标。词库不在本目录内，由 `product-lock.json` 钉住的发布产物在构建时下载。

## 构建与测试

产品构建通过 `server/CMakeLists.txt` 的 `add_subdirectory` 引入本目录，所以**改了引擎最省事的验证是跑 server 的构建与测试**，那也是 CI 实际走的路径。

要单独构建（迭代引擎本身时快得多）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure --timeout 20
```

`googlepinyinime-rev/src/share/` 和 `utfcpp/source` 现在都在树里，普通检出即可，不需要 `git submodule update`。

**改了引擎就要跑测试。** 引擎的缺陷会以"某个输入模式偶发不出候选"的形式出现在前端，编译通过说明不了什么。

### tests/ 是一个 CI 不构建的独立工程

`tests/CMakeLists.txt` 是独立 CMake 工程，产出 `imetest`（注册源只有 `tests/src/test_pinyin.cpp`）和全拼纠错离线评测工具 `eval_quanpin_autocorrect`，详见 [tests/README.md](tests/README.md)。它**不被根 `CMakeLists.txt` 引入**——根目录只把 `tests/src/*.cpp` 当自己测试目标的源文件，不 add_subdirectory(tests)。所以：

- `imetest` 里的用例**在 CI 中不会执行**；往 `test_pinyin.cpp` 加用例，PR 的绿勾只代表引擎仍能编译，不代表测试跑过
- 它是 MSVC 专有配置（Windows.h、`/Zc:__cplusplus`）；Boost 解析按显式 `-D` → 环境变量 → scoop 布局回退，不写死本机路径

新增测试请加到根 `CMakeLists.txt` 已登记的目标里（`tests/src/test_*_input_session.cpp` 那一批），那些才会被跑到。

## 全拼分表命名（跨仓硬约定）

`msime.db` 按音节数加首音节首字母分表。**权威定义是 `contracts/dictionary/format.json`**，C++ 生成头供查询/回放使用，Python API（仓库根的 `scripts/dictionary_product.py` 是同一份的副本）供校验发布的词库使用；设置页通过公共 `quanpin::build_table_name` 写入。修改后运行生成检查及七/八/九音节创建、查询、回放回归。

| 音节数 | 表名 | 示例 |
|---|---|---|
| 1–7 | `tbl_{N}_{首字母}` | `ni'hao` → `tbl_2_n` |
| ≥ 8 | `tbl_others_{首字母}` | `shui'shan'shu'ru'fa'hai'ke'yi` → `tbl_others_s` |

首字母取第一个音节的第一个小写拉丁字母。**禁止对 ≥8 音节拼出 `tbl_8_*`**：建库脚本不会创建这些表，写入和回放都会失败，安装升级时的回放失败会导致整批回滚并中止安装。改规则必须更新共享格式契约，禁止在消费端重新拼接表名。

## 用户词库权重

`user_dictionary/user_dictionary_journal.cpp` 的 `adjust_candidate_ranking` 改起来比看上去危险，几条已经踩过的坑：

- 权重要留在出货词典的量级内（`kManagedWeightCeiling`）。曾经有过一次局部 rebalance 写出 10^12 级数值，以及拿权重为 1 的生僻词当阶梯基准写出负权重到邻近 key 上
- 一个上下文里可能混着多个 `entry_key`（单字母简拼 `y` 会同时出现 `yi` 的「一」和 `you` 的「有」）。按 `entry_key` 过滤候选列表会让选中项 rank 恒为 0，早退分支直接返回，一个权重都不写
- 但写入必须按各候选自己的 key 走，否则会把权重写到别的 key 的行上

改这里之前先读懂现有的护栏注释，它们每一条都对应一个修过的缺陷。

## 切分与候选

- 平局时偏好更短的首音节（`quanpin/quanpin_utils.cpp` 的 `cut_one_piece_min_segments`）。**这个判据是对的，不要翻成前向最长匹配**——在 48068 条带词典切分的多音节词上量过，翻向会从 99.605% 掉到 99.393%，因为拼音的 `n`/`ng` 歧义让前向最长匹配在中文上不成立
- 歧义切分交给词典裁决，走 `quanpin_dictionary.cpp` 的备选切分路径，受 `kMaxSyllablesForMultipleSegmentations` 限制
- `query()` 在每次按键上都会跑。往里加无条件的枚举或查询前先量耗时，`tests/src/test_pinyin.cpp` 里那个 timings 用例只打印不断言，拦不住回归

## 数据

`msime.db`、`english.db`、`others.db`、`dict_japanese.dat` 都来自 `product-lock.json` 钉住的词库 Release，构建时下载；建库流水线不在本仓库内。查询侧用的格式契约是 `contracts/dictionary/format.json`，改它等于改一个已经发布的词库的读法，务必确认锁住的那一版词库仍然符合新规则。

候选窗的中英翻译除 `english.db` 的大表外还有一层人工覆盖：`english.db` 同目录的 `custom_translations.txt`，格式 `源<Tab>释义`，优先级高于大表（`english/english_dictionary.cpp`）。要纠正个别释义改这里，不要动 ECDICT 生成的大表。

## 提交

提交信息用 `type(scope): 摘要`。不要添加 `Co-Authored-By`、`Generated with` 或其他 AI 生成标记。
