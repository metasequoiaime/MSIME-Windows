# engine/tests

本目录产出两个独立二进制，都通过 `add_subdirectory` 链接 `engine/CMakeLists.txt` 的正式引擎库 `MetasequoiaIme::Engine`（不另抄源文件清单，编译选项与出货构建一致），互不依赖：

| 目标 | main | 用途 |
|---|---|---|
| `imetest` | `src/test_pinyin.cpp` | 引擎测试：全拼/双拼会话、退格、词典增删与查询时序；注册源只有 `test_pinyin.cpp`，CI 不执行，仅作本地跑测试的入口 |
| `eval_quanpin_autocorrect` | `src/eval_quanpin_autocorrect.cpp` | 全拼纠错离线评测：采样真实 msime.db 注错，测召回（R@1/R@3）、读音还原与温态延迟 |

## 构建

```powershell
cmake --preset default
cmake --build build --config Release --target <imetest | eval_quanpin_autocorrect>
```

两个目标共用同一份引擎库，只多编各自的 main。日常改代码跑 `imetest` 即可，按需单独构建评测目标。

## eval_quanpin_autocorrect：离线评测工具

这是**评测工具，不是测试**，永远进不了 CI：

- 依赖真实的 `msime.db`（用 `--db` 指定）和 `server/assets/tables` 资源目录（在仓库根目录运行，或 `--resource` 指定）。
- 不挂测试框架、不进断言，产出的是 markdown/CSV 报告，供人工对照基线。
- 存在的理由：纠错阈值、门逻辑（gate）或错误类型改动后，需要用同一把尺子复测。固定种子保证逐档样本一致，数字与历史基线可比。基线口径是「改动后各档不得低于基线值」的零回归门禁。

注错的变体生成规则（交换/邻键/漏字/多字、QWERTY 邻键表、长度与合法音节过滤）与 `server/scripts/generate_quanpin_autocorrect.py` **必须逐键一致**——两侧漂移会让注错模型脱离纠错表的假设空间，基线失去可比性。改任一侧时在同一个提交里同步另一侧。

`--seeds-file <路径>` 是独立数据入口：每行 `typo<TAB>期望key[<TAB>期望词]`（`#` 注释、空行跳过，期望词缺省时取该 key 的最高权重词），产出独立的 Seed suite 报告节与 `seeds` 聚合 CSV 行。它**豁免于上述逐键一致契约**——种子表度量的正是契约外与用户反馈直接相关的场景（误报/漏报样例固化）。

资源目录要求：引擎加载 `sc.lm`（词格）与 `sentence-model-*.safetensors`（神经重排），而 `server/assets/tables` 只有 `dict_pinyin.dat`、`helpcode.txt`、`user_dict.dat`——模型文件不入库。本机没有把模型放进 tables 时，用安装数据目录拼一个隔离资源目录（只读模型拷贝 + `msime.db` 副本），`--db` 与 `--resource` 都指过去；直接指安装目录也可以，但评测进程会以读写方式打开 `--db`，拷贝副本更干净。

沙箱环境下子进程可能被拒绝在 `%TEMP%` 下新建目录（表现为 `create_directories: Access is denied.` 后直接退出），把 `TEMP`/`TMP` 环境变量指到仓库内的已存在目录再跑即可。

历史基线与采集笔记在归档任务目录：`.trellis/tasks/archive/2026-09/09-12-quanpin-autocorrect-patent/eval/`（阶段 0 基线）与 `09-13-quanpin-insertion-autocorrect/eval/`（insertion 上线前后对照）。

### 运行

```powershell
# 仓库根目录执行；--model 可选 mixed|deletion|ambiguous|insertion|outside
# outside = 表外形状注错（非相邻替换/远键插入），度量生成式纠错空间的覆盖增益，
# 与静态表形状构造性不相交——该档 R@1 在无生成空间的代码上恒为 0。
./engine/tests/build/bin/Release/eval_quanpin_autocorrect.exe `
    --db <msime.db 路径> `
    --samples 300 --seed 42 --model mixed `
    --csv eval-mixed.csv > eval-mixed.md
```

参数细节与报告分节说明见 `src/eval_quanpin_autocorrect.cpp` 文件头注释。
