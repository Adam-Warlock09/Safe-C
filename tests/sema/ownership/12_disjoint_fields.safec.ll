%struct.P = type { i32, i32 }

declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @f(ptr %p) {
entry:
  %p.addr = alloca ptr
  store ptr %p, ptr %p.addr
  %x.addr = alloca ptr
    br label %body

body:
  %t1 = load ptr, ptr %p.addr
  %t2 = getelementptr inbounds %struct.P, ptr %t1, i32 0, i32 0
  store ptr %t2, ptr %x.addr
  %t3 = load ptr, ptr %p.addr
  %t4 = getelementptr inbounds %struct.P, ptr %t3, i32 0, i32 1
  store i32 1, ptr %t4
  %t5 = load ptr, ptr %x.addr
  store i32 2, ptr %t5
  ret void
}
