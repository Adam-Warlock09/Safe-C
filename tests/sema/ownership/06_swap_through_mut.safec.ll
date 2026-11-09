%struct.Node = type { i32, ptr }
%struct.List = type { ptr }

declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @push(ptr %l, i32 %v) {
entry:
  %l.addr = alloca ptr
  store ptr %l, ptr %l.addr
  %v.addr = alloca i32
  store i32 %v, ptr %v.addr
  %n.addr = alloca ptr
  %old.addr = alloca ptr
    br label %body

body:
  %t1 = getelementptr %struct.Node, ptr null, i32 1
  %t2 = ptrtoint ptr %t1 to i64
  %t3 = call ptr @malloc(i64 %t2)
  store ptr %t3, ptr %n.addr
  %t4 = load ptr, ptr %n.addr
  %t5 = getelementptr inbounds %struct.Node, ptr %t4, i32 0, i32 0
  %t6 = load i32, ptr %v.addr
  store i32 %t6, ptr %t5
  %t7 = load ptr, ptr %l.addr
  %t8 = getelementptr inbounds %struct.List, ptr %t7, i32 0, i32 0
  %t9 = load ptr, ptr %t8
  store ptr %t9, ptr %old.addr
  %t10 = load ptr, ptr %n.addr
  %t11 = getelementptr inbounds %struct.Node, ptr %t10, i32 0, i32 1
  %t12 = load ptr, ptr %old.addr
  store ptr %t12, ptr %t11
  %t13 = load ptr, ptr %l.addr
  %t14 = getelementptr inbounds %struct.List, ptr %t13, i32 0, i32 0
  %t15 = load ptr, ptr %n.addr
  store ptr %t15, ptr %t14
  ret void
}
