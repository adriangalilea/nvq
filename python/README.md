# nvq

NVIDIA cards as JSON, for programs. This package carries the [nvq](https://github.com/adriangalilea/nvq) binary for linux x86_64 and aarch64 (glibc 2.17 or newer) and runs it: `pip install nvq` is the whole install, and `nvq` lands on PATH too.

```python
import nvq

n = nvq.NVQ()
for card in n.list()["cards"]:
    print(card["model"], card["state"], card.get("tempC"))

for ev in n.watch(every=1.0):
    if ev["event"] == "xid":
        print(ev["xid"], ev["meaning"])
```

Documents are the parsed JSON, keys as the [contract](https://github.com/adriangalilea/nvq/blob/main/schema/nvq.schema.json) names them; a value the card cannot report is an absent key. Their types (`nvq.documents`) are TypedDicts generated from that schema. A command that failed as a whole raises `nvq.Error` with its `code` and `exit`; a lost card or a failed probe step is data, not an exception. Each call takes a `timeout`: past it nvq is killed, so a card hung in the driver never hangs your program.
