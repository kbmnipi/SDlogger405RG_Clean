import io
p = r"doc\MILESTONE_1_SUCCESS.md"
lines = io.open(p, encoding="utf-8").read().split("\n")
for i in range(0, 60):
    print("%3d| %s" % (i+1, lines[i].encode("ascii","backslashreplace").decode("ascii")))
