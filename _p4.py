import io
lines = io.open(r"doc\MILESTONE_1_SUCCESS.md", encoding="utf-8").read().split("\n")
print("TOTAL", len(lines))
for i in range(340, len(lines)):
    print("%3d| %s" % (i+1, lines[i].encode("ascii","backslashreplace").decode("ascii")))
