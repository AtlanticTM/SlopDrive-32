---
paths:
  - "platformio.ini"
---

# Serena / clangd navigation upkeep

Serena MCP (.mcp.json, ide-assistant context) and clangd both navigate this
tree through compile_commands.json. Whenever platformio.ini changes (envs,
build flags, lib_deps), regenerate it in the same session:

```
python -m platformio run -t compiledb -e s3_main
```

Stale compile databases produce confidently wrong symbol resolution, which is
worse than none.
