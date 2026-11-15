import io
p = r"doc\MILESTONE_1_SUCCESS.md"
lines = io.open(p, encoding="utf-8").read().split("\n")
for i in list(range(59, 72)) + list(range(100, 118)) + list(range(300, 318)):
    print("%3d| %s" % (i+1, lines[i].encode("ascii","backslashreplace").decode("ascii")))
print("TOTAL:", len(lines))
