
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
  %t1 = getelementptr i32, ptr null, i32 1
  %t2 = ptrtoint ptr %t1 to i64
  %t3 = call ptr @malloc(i64 %t2)
  store ptr %t3, ptr %a.addr
  %t4 = load i32, ptr %c.addr
  switch i32 %t4, label %sw.default.3 [
    i32 1, label %sw.case.2
  ]
sw.case.2:
  %t5 = load ptr, ptr %a.addr
  call void @free(ptr %t5)
  br label %sw.end.1
sw.default.3:
  %t6 = load ptr, ptr %a.addr
  call void @free(ptr %t6)
  br label %sw.end.1
sw.end.1:
  ret void
}
