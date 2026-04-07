# LevOJ-Solver

> 自动获取 NUIST LevOJ 平台的AI题解, 解题思路以及当前系统提示词. 爆破提示词如开源.

## 🗺️ Visual Overview (Code & Logic Map)

```mermaid
graph TD
    subgraph "User / Local File"
        Input["User Input (courseId/problemId)"]
        Cookie["cookie.txt"]
    end

    subgraph "LevOJ-Solver (src/main.cpp)"
        Main["main()"]
        Config["build_hardcoded_config()"]
        SSE_Handler["Chunk Handler (Lambda)"]
        Collect["collect_sse_content()"]
        Extract["extract_content()"]
        CodeBlock["extract_code_blocks()"]
    end

    subgraph "Remote AI Service"
        API["*.*.*.* /v1/ai/chat/stream"]
    end

    Input --> Main
    Cookie -->|"read_text_file()"| Main
    Main -->|"HTTP POST (httplib)"| Config
    Config -->|"Payload + Cookie"| API
    API -->|"SSE Stream Chunk"| SSE_Handler
    SSE_Handler -->|"Parse Event"| Collect
    Collect -->|"Parse JSON (nlohmann)"| Extract
    Extract -->|"Concat Strings"| Main
    Main -->|"Regex Match"| CodeBlock
    CodeBlock -->|"Print Code"| Output["Console Output"]

    style API fill:#ffebee,color:#b71c1c
    style Main fill:#e3f2fd,color:#0d47a1
    style Output fill:#e8f5e9,color:#1b5e20
```

## 🛠️ Tools & Dependencies

- C++ 17
- httplib
- nlohmann/json

## 🔄️ Usage

1. 获取你的 NUIST LevOJ 平台的 Cookie 并保存到 `cookie.txt` 文件中。
2. 运行 `LevOJ-Solver` 命令。
3. 根据提示输入课程 ID和问题 ID。

---
> [!NOTE]
>
> 如果你想自己编译运行

1. 克隆项目到本地.
2. 修改 `src/main.cpp` 中的 `HOST` 为合适的IP地址.
3. 修改 `src/main.cpp` 中的 `query_body` 为合适的提示词.

4. 编译并运行:
   ```
   xmake build; xmake run
   ```
