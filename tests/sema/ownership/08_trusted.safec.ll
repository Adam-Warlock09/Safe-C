
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @f() {
entry:
  %a.addr = alloca ptr
    br label %body

body:
  %t1 = getelementptr i32, ptr null, i32 1
  %t2 = ptrtoint ptr %t1 to i64
  %t3 = call ptr @malloc(i64 %t2)
  store ptr %t3, ptr %a.addr
  %t4 = load ptr, ptr %a.addr
  call void @free(ptr %t4)
  ret void
}
