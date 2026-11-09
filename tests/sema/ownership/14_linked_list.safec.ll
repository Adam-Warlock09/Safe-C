%struct.Node = type { i32, ptr }
%struct.List = type { ptr, i32 }

declare ptr @malloc(i64)
declare void @free(ptr)
declare void @llvm.trap()
declare i32 @printf(ptr, ...)



define void @free_list(ptr %n) {
entry:
  %n.addr = alloca ptr
  store ptr %n, ptr %n.addr
  %nx.addr = alloca ptr
    br label %body

body:
  br label %while.cond.1
while.cond.1:
  %t1 = load ptr, ptr %n.addr
  %t2 = icmp ne ptr %t1, null
  br i1 %t2, label %while.body.2, label %while.end.3
while.body.2:
  %t3 = load ptr, ptr %n.addr
  %t4 = getelementptr inbounds %struct.Node, ptr %t3, i32 0, i32 1
  %t5 = load ptr, ptr %t4
  store ptr %t5, ptr %nx.addr
  %t6 = load ptr, ptr %n.addr
  %t7 = getelementptr inbounds %struct.Node, ptr %t6, i32 0, i32 1
  store ptr null, ptr %t7
  %t8 = load ptr, ptr %n.addr
  call void @free(ptr %t8)
  %t9 = load ptr, ptr %nx.addr
  store ptr %t9, ptr %n.addr
  br label %while.cond.1
while.end.3:
  ret void
}

define void @push(ptr %l, i32 %v) {
entry:
  %l.addr = alloca ptr
  store ptr %l, ptr %l.addr
  %v.addr = alloca i32
  store i32 %v, ptr %v.addr
  %n.addr = alloca ptr
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
  %t7 = load ptr, ptr %n.addr
  %t8 = getelementptr inbounds %struct.Node, ptr %t7, i32 0, i32 1
  %t9 = load ptr, ptr %l.addr
  %t10 = getelementptr inbounds %struct.List, ptr %t9, i32 0, i32 0
  %t11 = load ptr, ptr %t10
  store ptr %t11, ptr %t8
  %t12 = load ptr, ptr %l.addr
  %t13 = getelementptr inbounds %struct.List, ptr %t12, i32 0, i32 0
  %t14 = load ptr, ptr %n.addr
  store ptr %t14, ptr %t13
  %t15 = load ptr, ptr %l.addr
  %t16 = getelementptr inbounds %struct.List, ptr %t15, i32 0, i32 1
  %t17 = load ptr, ptr %l.addr
  %t18 = getelementptr inbounds %struct.List, ptr %t17, i32 0, i32 1
  %t19 = load i32, ptr %t18
  %t20 = add nsw i32 %t19, 1
  store i32 %t20, ptr %t16
  ret void
}

define ptr @pop_node(ptr %l) {
entry:
  %l.addr = alloca ptr
  store ptr %l, ptr %l.addr
  %h.addr = alloca ptr
    br label %body

body:
  %t1 = load ptr, ptr %l.addr
  %t2 = getelementptr inbounds %struct.List, ptr %t1, i32 0, i32 0
  %t3 = load ptr, ptr %t2
  store ptr %t3, ptr %h.addr
  %t4 = load ptr, ptr %h.addr
  %t5 = icmp eq ptr %t4, null
  br i1 %t5, label %if.then.1, label %if.end.3
if.then.1:
  %t6 = load ptr, ptr %l.addr
  %t7 = getelementptr inbounds %struct.List, ptr %t6, i32 0, i32 0
  store ptr null, ptr %t7
  ret ptr null
if.end.3:
  %t8 = load ptr, ptr %l.addr
  %t9 = getelementptr inbounds %struct.List, ptr %t8, i32 0, i32 0
  %t10 = load ptr, ptr %h.addr
  %t11 = getelementptr inbounds %struct.Node, ptr %t10, i32 0, i32 1
  %t12 = load ptr, ptr %t11
  store ptr %t12, ptr %t9
  %t13 = load ptr, ptr %h.addr
  %t14 = getelementptr inbounds %struct.Node, ptr %t13, i32 0, i32 1
  store ptr null, ptr %t14
  %t15 = load ptr, ptr %l.addr
  %t16 = getelementptr inbounds %struct.List, ptr %t15, i32 0, i32 1
  %t17 = load ptr, ptr %l.addr
  %t18 = getelementptr inbounds %struct.List, ptr %t17, i32 0, i32 1
  %t19 = load i32, ptr %t18
  %t20 = sub nsw i32 %t19, 1
  store i32 %t20, ptr %t16
  %t21 = load ptr, ptr %h.addr
  ret ptr %t21
}

define i32 @sum(ptr %l) {
entry:
  %l.addr = alloca ptr
  store ptr %l, ptr %l.addr
  %s.addr = alloca i32
  %cur.addr = alloca ptr
    br label %body

body:
  store i32 0, ptr %s.addr
  %t1 = load ptr, ptr %l.addr
  %t2 = getelementptr inbounds %struct.List, ptr %t1, i32 0, i32 0
  %t3 = load ptr, ptr %t2
  store ptr %t3, ptr %cur.addr
  br label %while.cond.1
while.cond.1:
  %t4 = load ptr, ptr %cur.addr
  %t5 = icmp ne ptr %t4, null
  br i1 %t5, label %while.body.2, label %while.end.3
while.body.2:
  %t6 = load i32, ptr %s.addr
  %t7 = load ptr, ptr %cur.addr
  %t8 = getelementptr inbounds %struct.Node, ptr %t7, i32 0, i32 0
  %t9 = load i32, ptr %t8
  %t10 = add nsw i32 %t6, %t9
  store i32 %t10, ptr %s.addr
  %t11 = load ptr, ptr %cur.addr
  %t12 = getelementptr inbounds %struct.Node, ptr %t11, i32 0, i32 1
  %t13 = load ptr, ptr %t12
  store ptr %t13, ptr %cur.addr
  br label %while.cond.1
while.end.3:
  %t14 = load i32, ptr %s.addr
  ret i32 %t14
}

define void @clear(ptr %l) {
entry:
  %l.addr = alloca ptr
  store ptr %l, ptr %l.addr
  %n.addr = alloca ptr
    br label %body

body:
  %t1 = load ptr, ptr %l.addr
  %t2 = getelementptr inbounds %struct.List, ptr %t1, i32 0, i32 0
  %t3 = load ptr, ptr %t2
  store ptr %t3, ptr %n.addr
  %t4 = load ptr, ptr %l.addr
  %t5 = getelementptr inbounds %struct.List, ptr %t4, i32 0, i32 0
  store ptr null, ptr %t5
  %t6 = load ptr, ptr %n.addr
  call void @free_list(ptr %t6)
  %t7 = load ptr, ptr %l.addr
  %t8 = getelementptr inbounds %struct.List, ptr %t7, i32 0, i32 1
  store i32 0, ptr %t8
  ret void
}

define i32 @main() {
entry:
  %l.addr = alloca %struct.List
  %s.addr = alloca i32
  %rest.addr = alloca ptr
    br label %body

body:
  %t1 = insertvalue %struct.List undef, ptr null, 0
  %t2 = insertvalue %struct.List %t1, i32 0, 1
  store %struct.List %t2, ptr %l.addr
  call void @push(ptr %l.addr, i32 1)
  call void @push(ptr %l.addr, i32 2)
  %t3 = call ptr @pop_node(ptr %l.addr)
  call void @free_list(ptr %t3)
  %t4 = call i32 @sum(ptr %l.addr)
  store i32 %t4, ptr %s.addr
  call void @clear(ptr %l.addr)
  %t5 = getelementptr inbounds %struct.List, ptr %l.addr, i32 0, i32 0
  %t6 = load ptr, ptr %t5
  store ptr %t6, ptr %rest.addr
  %t7 = getelementptr inbounds %struct.List, ptr %l.addr, i32 0, i32 0
  store ptr null, ptr %t7
  %t8 = load ptr, ptr %rest.addr
  call void @free_list(ptr %t8)
  %t9 = load i32, ptr %s.addr
  %t10 = icmp eq i32 %t9, 1
  br i1 %t10, label %if.then.1, label %if.end.3
if.then.1:
  ret i32 0
if.end.3:
  ret i32 1
}
