import io
lines = io.open(r"doc\MILESTONE_1_SUCCESS.md", encoding="utf-8").read().split("\n")
for i in range(230, 340):
    print("%3d| %s" % (i+1, lines[i].encode("ascii","backslashreplace").decode("ascii")))
