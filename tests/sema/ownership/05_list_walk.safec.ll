%struct.Node = type { i32, ptr }

declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define i32 @sum(ptr %n) {
entry:
  %n.addr = alloca ptr
  store ptr %n, ptr %n.addr
  %s.addr = alloca i32
  %cur.addr = alloca ptr
    br label %body

body:
  store i32 0, ptr %s.addr
  %t1 = load ptr, ptr %n.addr
  store ptr %t1, ptr %cur.addr
  br label %while.cond.1
while.cond.1:
  %t2 = load ptr, ptr %cur.addr
  %t3 = icmp ne ptr %t2, null
  br i1 %t3, label %while.body.2, label %while.end.3
while.body.2:
  %t4 = load i32, ptr %s.addr
  %t5 = load ptr, ptr %cur.addr
  %t6 = getelementptr inbounds %struct.Node, ptr %t5, i32 0, i32 0
  %t7 = load i32, ptr %t6
  %t8 = add nsw i32 %t4, %t7
  store i32 %t8, ptr %s.addr
  %t9 = load ptr, ptr %cur.addr
  %t10 = getelementptr inbounds %struct.Node, ptr %t9, i32 0, i32 1
  %t11 = load ptr, ptr %t10
  store ptr %t11, ptr %cur.addr
  br label %while.cond.1
while.end.3:
  %t12 = load i32, ptr %s.addr
  ret i32 %t12
}
