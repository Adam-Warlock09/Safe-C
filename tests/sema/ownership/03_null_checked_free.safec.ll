
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @f(ptr %p) {
entry:
  %p.addr = alloca ptr
  store ptr %p, ptr %p.addr
    br label %body

body:
  %t1 = load ptr, ptr %p.addr
  %t2 = icmp ne ptr %t1, null
  br i1 %t2, label %if.then.1, label %if.end.3
if.then.1:
  %t3 = load ptr, ptr %p.addr
  call void @free(ptr %t3)
  br label %if.end.3
if.end.3:
  ret void
}
