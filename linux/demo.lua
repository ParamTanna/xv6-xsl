-- a small Lua program, run by the Lua interpreter (a Linux binary) on xv6

print("Lua " .. _VERSION:sub(5) .. " running on " .. (io.popen and "xv6" or "?"))

-- tables and functions
local function fib(n)
  if n < 2 then return n end
  return fib(n - 1) + fib(n - 2)
end
local t = {}
for i = 1, 15 do t[#t + 1] = fib(i) end
print("fibonacci: " .. table.concat(t, " "))

-- strings and floating point (needs the FPU)
print(string.format("pi = %.6f, sqrt(2) = %.6f", math.pi, math.sqrt(2)))
print(("hello from lua"):upper():rep(2, " | "))

-- closures
local function counter()
  local c = 0
  return function() c = c + 1; return c end
end
local next_id = counter()
next_id(); next_id()
print("closure counter: " .. next_id())

-- files through Linux openat/read/write/close on the xv6 file system
local f = assert(io.open("lua.txt", "w"))
f:write("line one\n", "line two\n")
f:close()
local n = 0
for line in io.lines("lua.txt") do n = n + 1 end
print("wrote and read back lua.txt: " .. n .. " lines")
os.remove("lua.txt")

-- time (clock_gettime)
local start = os.clock()
local s = 0
for i = 1, 200000 do s = s + i end
print(string.format("sum 1..200000 = %d, cpu time %.2f s", s, os.clock() - start))
print("os.time() = " .. os.time())
