
declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @g(ptr %p) {
entry:
  %p.addr = alloca ptr
  store ptr %p, ptr %p.addr
    br label %body

body:
  %t1 = load ptr, ptr %p.addr
  store i32 1, ptr %t1
  ret void
}

define void @f() {
entry:
  %x.addr = alloca i32
    br label %body

body:
  store i32 0, ptr %x.addr
  call void @g(ptr %x.addr)
  call void @g(ptr %x.addr)
  ret void
}
