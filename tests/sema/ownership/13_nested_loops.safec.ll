
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @f(i32 %n) {
entry:
  %n.addr = alloca i32
  store i32 %n, ptr %n.addr
  %p.addr = alloca ptr
    br label %body

body:
  store ptr null, ptr %p.addr
  br label %while.cond.1
while.cond.1:
  %t1 = load i32, ptr %n.addr
  br i1 %t1, label %while.body.2, label %while.end.3
while.body.2:
  br label %while.cond.4
while.cond.4:
  %t2 = load i32, ptr %n.addr
  br i1 %t2, label %while.body.5, label %while.end.6
while.body.5:
  %t3 = load ptr, ptr %p.addr
  %t4 = icmp ne ptr %t3, null
  br i1 %t4, label %if.then.7, label %if.end.9
if.then.7:
  %t5 = load ptr, ptr %p.addr
  call void @free(ptr %t5)
  br label %if.end.9
if.end.9:
  %t6 = getelementptr i32, ptr null, i32 1
  %t7 = ptrtoint ptr %t6 to i64
  %t8 = call ptr @malloc(i64 %t7)
  store ptr %t8, ptr %p.addr
  br label %while.cond.4
while.end.6:
  br label %while.cond.1
while.end.3:
  %t9 = load ptr, ptr %p.addr
  %t10 = icmp ne ptr %t9, null
  br i1 %t10, label %if.then.10, label %if.end.12
if.then.10:
  %t11 = load ptr, ptr %p.addr
  call void @free(ptr %t11)
  br label %if.end.12
if.end.12:
  ret void
}
