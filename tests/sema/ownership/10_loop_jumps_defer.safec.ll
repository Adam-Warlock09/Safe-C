
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @f(i32 %c) {
entry:
  %c.addr = alloca i32
  store i32 %c, ptr %c.addr
  %a.addr = alloca ptr
    br label %body

body:
  br label %while.cond.1
while.cond.1:
  %t1 = load i32, ptr %c.addr
  br i1 %t1, label %while.body.2, label %while.end.3
while.body.2:
  %t2 = getelementptr i32, ptr null, i32 1
  %t3 = ptrtoint ptr %t2 to i64
  %t4 = call ptr @malloc(i64 %t3)
  store ptr %t4, ptr %a.addr
  %t5 = load i32, ptr %c.addr
  br i1 %t5, label %if.then.4, label %if.end.6
if.then.4:
  %t6 = load ptr, ptr %a.addr
  call void @free(ptr %t6)
  br label %while.end.3
if.end.6:
  %t7 = load i32, ptr %c.addr
  br i1 %t7, label %if.then.7, label %if.end.9
if.then.7:
  %t8 = load ptr, ptr %a.addr
  call void @free(ptr %t8)
  br label %while.cond.1
if.end.9:
  %t9 = load ptr, ptr %a.addr
  %t10 = load i32, ptr %c.addr
  store i32 %t10, ptr %t9
  %t11 = load ptr, ptr @a
  call void @free(ptr %t11)
  br label %while.cond.1
while.end.3:
  ret void
}
