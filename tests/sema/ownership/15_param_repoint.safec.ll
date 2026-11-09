
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @f(ptr %p) {
entry:
  %p.addr = alloca ptr
  store ptr %p, ptr %p.addr
  %x.addr = alloca i32
    br label %body

body:
  store i32 1, ptr %x.addr
  store ptr %x.addr, ptr %p.addr
  %t1 = load ptr, ptr %p.addr
  store i32 2, ptr %t1
  ret void
}
