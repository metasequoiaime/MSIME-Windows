# engine/tests

本目录产出两个独立二进制，共用 `CMakeLists.txt` 里的引擎源列表，互不依赖：

| 目标 | main | 用途 |
|---|---|---|
| `imetest` | `src/test_pinyin.cpp` | 引擎测试：全拼/双拼会话、退格、词典增删与查询时序；注册源只有 `test_pinyin.cpp`，CI 不执行，仅作本地跑测试的入口 |
| `eval_quanpin_autocorrect` | `src/eval_quanpin_autocorrect.cpp` | 全拼纠错离线评测：采样真实 msime.db 注错，测召回（R@1/R@3）、读音还原与温态延迟 |

## 构建

```powershell
cmake --preset default
cmake --build build --config Release --target <imetest | eval_quanpin_autocorrect>
```

`eval_quanpin_autocorrect` 会把引擎源完整再编一遍（接近翻倍构建时间），日常改代码跑 `imetest` 即可，按需单独构建评测目标。

## eval_quanpin_autocorrect：离线评测工具

这是**评测工具，不是测试**，永远进不了 CI：

- 依赖真实的 `msime.db`（用 `--db` 指定）和 `server/assets/tables` 资源目录（在仓库根目录运行，或 `--resource` 指定）。
- 不挂测试框架、不进断言，产出的是 markdown/CSV 报告，供人工对照基线。
- 存在的理由：纠错阈值、门逻辑（gate）或错误类型改动后，需要用同一把尺子复测。固定种子保证逐档样本一致，数字与历史基线可比。基线口径是「改动后各档不得低于基线值」的零回归门禁。

注错的变体生成规则（交换/邻键/漏字/多字、QWERTY 邻键表、长度与合法音节过滤）与 `server/scripts/generate_quanpin_autocorrect.py` **必须逐键一致**——两侧漂移会让注错模型脱离纠错表的假设空间，基线失去可比性。改任一侧时在同一个提交里同步另一侧。

历史基线与采集笔记在归档任务目录：`.trellis/tasks/archive/2026-09/09-12-quanpin-autocorrect-patent/eval/`（阶段 0 基线）与 `09-13-quanpin-insertion-autocorrect/eval/`（insertion 上线前后对照）。

### 运行

```powershell
# 仓库根目录执行；--model 可选 mixed|deletion|ambiguous|insertion
./engine/tests/build/bin/Release/eval_quanpin_autocorrect.exe `
    --db <msime.db 路径> `
    --samples 300 --seed 42 --model mixed `
    --csv eval-mixed.csv > eval-mixed.md
```

参数细节与报告分节说明见 `src/eval_quanpin_autocorrect.cpp` 文件头注释。
