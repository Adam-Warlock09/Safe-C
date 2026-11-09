%struct.Node = type { i32, ptr }

declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @free_list(ptr %n) {
entry:
  %n.addr = alloca ptr
  store ptr %n, ptr %n.addr
  %nx.addr = alloca ptr
    br label %body

body:
  br label %while.cond.1
while.cond.1:
  %t1 = load ptr, ptr %n.addr
  %t2 = icmp ne ptr %t1, null
  br i1 %t2, label %while.body.2, label %while.end.3
while.body.2:
  %t3 = load ptr, ptr %n.addr
  %t4 = getelementptr inbounds %struct.Node, ptr %t3, i32 0, i32 1
  %t5 = load ptr, ptr %t4
  store ptr %t5, ptr %nx.addr
  %t6 = load ptr, ptr %n.addr
  %t7 = getelementptr inbounds %struct.Node, ptr %t6, i32 0, i32 1
  store ptr null, ptr %t7
  %t8 = load ptr, ptr %n.addr
  call void @free(ptr %t8)
  %t9 = load ptr, ptr %nx.addr
  store ptr %t9, ptr %n.addr
  br label %while.cond.1
while.end.3:
  ret void
}
