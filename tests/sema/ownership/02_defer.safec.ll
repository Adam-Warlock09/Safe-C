
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @f(i32 %c) {
entry:
  %c.addr = alloca i32
  store i32 %c, ptr %c.addr
  %x.addr = alloca ptr
    br label %body

body:
  %t1 = getelementptr i32, ptr null, i32 1
  %t2 = ptrtoint ptr %t1 to i64
  %t3 = call ptr @malloc(i64 %t2)
  store ptr %t3, ptr %x.addr
  %t4 = load i32, ptr %c.addr
  br i1 %t4, label %if.then.1, label %if.end.3
if.then.1:
  %t5 = load ptr, ptr %x.addr
  call void @free(ptr %t5)
  ret void
if.end.3:
  %t6 = load ptr, ptr %x.addr
  store i32 3, ptr %t6
  ret void
}
