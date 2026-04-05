import re

Import("env")

with open("CHANGELOG.md") as f:
    for line in f:
        m = re.match(r"## \[(\d+\.\d+\.\d+)\]", line)
        if m:
            version = m.group(1)
            env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
            print(f"  FW_VERSION = {version}")
            break
