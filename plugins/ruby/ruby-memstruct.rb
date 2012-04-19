module DFHack
module MemHack
class MemStruct
	attr_accessor :_memaddr
	def _at(addr) ; @_memaddr = addr ; dup ; end
	def _get ; self ; end
end

class Compound < MemStruct
	class << self
		attr_accessor :_fields
		def field(name, offset)
			struct = yield
			@_fields ||= []
			@_fields << [name, offset, struct]
			define_method(name) { struct._at(@_memaddr+offset)._get }
			define_method("#{name}=") { |v| struct._at(@_memaddr+offset)._set(v) }
		end

		def number(bits, signed)
			Number.new(bits, signed)
		end
		def float
			Float.new
		end
		def bit(shift)
			BitField.new(shift, 1)
		end
		def bits(shift, len)
			BitField.new(shift, len)
		end
		def pointer
			Pointer.new((yield if block_given?))
		end
		def pointer_ary(tglen)
			PointerAry.new(tglen, yield)
		end
		def static_array(len, tglen)
			StaticArray.new(tglen, len, yield)
		end
		def static_string(len)
			StaticString.new(len)
		end

		def stl_vector(tglen=nil)
			tg = yield if tglen
			case tglen
			when 1; StlVector8.new(tg)
			when 2; StlVector16.new(tg)
			else StlVector32.new(tg)
			end
		end
		def stl_string
			StlString.new
		end
		def stl_bit_vector
			StlBitVector.new
		end
		def stl_deque(tglen=nil)
			StlDeque.new(tglen, (yield if tglen))
		end

		def df_flagarray
			DfFlagarray.new
		end
		def df_array(tglen=nil)
			DfArray.new(tglen, (yield if tglen))
		end
		def df_linked_list
			DfLinkedList.new((yield if block_given?))
		end

		def global(glob)
			Global.new(glob)
		end
		def compound(&b)
			m = Class.new(Compound)
			m.instance_eval(&b)
			m.new
		end
		def rtti_classname(n)
			# TODO store total size for allocate() ? what about non-virtual ones ?
			DFHack.rtti_register(n, self)
		end
	end
	def _set(h) ; h.each { |k, v| send("_#{k}=", v) } ; end
end

class Number < MemStruct
	attr_accessor :_bits, :_signed
	def initialize(bits, signed)
		@_bits = bits
		@_signed = signed
	end

	def _get
		v = case @_bits
		when 32; DFHack.memory_read_int32(@_memaddr)
		when 16; DFHack.memory_read_int16(@_memaddr)
		when 8;  DFHack.memory_read_int8( @_memaddr)
		when 64;(DFHack.memory_read_int32(@_memaddr) & 0xffffffff) + (DFHack.memory_read_int32(@_memaddr+4) << 32)
		end
		v &= (1 << @_bits) - 1 if not @_signed
		v
	end

	def _set(v)
		case @_bits
		when 32; DFHack.memory_write_int32(@_memaddr, v)
		when 16; DFHack.memory_write_int16(@_memaddr, v)
		when 8;  DFHack.memory_write_int8( @_memaddr, v)
		when 64; DFHack.memory_write_int32(@_memaddr, v & 0xffffffff) ; DFHack.memory_write_int32(@memaddr+4, v>>32)
		end
	end
end
class Float < MemStruct
	# _get/_set defined in ruby.cpp
	def _get
		DFHack.memory_read_float(@_memaddr)
	end

	def _set(v)
		DFHack.memory_write_float(@_memaddr, v)
	end
end
class BitField < MemStruct
	attr_accessor :_shift, :_len
	def initialize(shift, len)
		@_shift = shift
		@_len = len
	end
	def _mask
		(1 << @_len) - 1
	end

	def _get
		v = DFHack.memory_read_int32(@_memaddr) >> @_shift
		if @_len == 1
			((v & 1) == 0) ? false : true
		else
			v & _mask
		end
	end

	def _set(v)
		if @_len == 1
			# allow 'bit = 0'
			v = (v && v != 0 ? 1 : 0)
		end
		v = (v & _mask) << @_shift

		ori = DFHack.memory_read_int32(@_memaddr) & 0xffffffff
		DFHack.memory_write_int32(@_memaddr, ori - (ori & ((-1 & _mask) << @_shift)) + v)
	end
end

class Pointer < MemStruct
	attr_accessor :_tg
	def initialize(tg)
		@_tg = tg
	end

	def _getp
		DFHack.memory_read_int32(@_memaddr) & 0xffffffff
	end

	# the pointer is invisible, forward all methods to the pointed object
	def method_missing(*a)
		addr = _getp
		tg = (addr == 0 ? nil : @_tg._at(addr)._get)
		tg.send(*a)
	end
end
class PointerAry < MemStruct
	attr_accessor :_tglen, :_tg
	def initialize(tglen, tg)
		@_tglen = tglen
		@_tg = tg
	end

	def _getp(i=0)
		delta = (i != 0 ? i*@_tglen : 0)
		(DFHack.memory_read_int32(@_memaddr) & 0xffffffff) + delta
	end

	def [](i)
		addr = _getp(i)
		return if addr == 0
		@_tg._at(addr)._get
	end
	def []=(i, v)
		addr = _getp(i)
		raise 'null pointer' if addr == 0
		@_tg._at(addr)._set(v)
	end
end
class StaticArray < MemStruct
	attr_accessor :_tglen, :_length, :_tg
	def initialize(tglen, length, tg)
		@_tglen = tglen
		@_length = length
		@_tg = tg
	end
	def _set(a)
		a.each_with_index { |v, i| self[i] = v }
	end
	alias length _length
	alias size _length
	def _tgat(i)
		@_tg._at(@_memaddr + i*@_tglen) if i >= 0 and i < @_length
	end
	def [](i)
		i += @_length if i < 0
		_tgat(i)._get
	end
	def []=(i, v)
		i += @_length if i < 0
		_tgat(i)._set(v)
	end

	include Enumerable
	def each ; (0...length).each { |i| yield self[i] } ; end
end
class StaticString < MemStruct
	attr_accessor :_length
	def initialize(length)
		@_length = length
	end
	def _get
		DFHack.memory_read(@_memaddr, @_length)
	end
	def _set(v)
		DFHack.memory_write(@_memaddr, v[0, @_length])
	end
end

class StlVector32 < MemStruct
	attr_accessor :_tg
	def initialize(tg)
		@_tg = tg
	end

	def length
		DFHack.memory_vector32_length(@_memaddr)
	end
	def size ; length ; end	# alias wouldnt work for subclasses
	def valueptr_at(idx)
		DFHack.memory_vector32_ptrat(@_memaddr, idx)
	end
	def insert_at(idx, val)
		DFHack.memory_vector32_insert(@_memaddr, idx, val)
	end
	def delete_at(idx)
		DFHack.memory_vector32_delete(@_memaddr, idx)
	end

	def _set(v)
		delete_at(length-1) while length > v.length	# match lengthes
		v.each_with_index { |e, i| self[i] = e }	# patch entries
	end

	def clear
		delete_at(length-1) while length > 0
	end
	def [](idx)
		idx += length if idx < 0
		@_tg._at(valueptr_at(idx))._get if idx >= 0 and idx < length
	end
	def []=(idx, v)
		idx += length if idx < 0
		if idx >= length
			insert_at(idx, 0)
		elsif idx < 0
			raise 'invalid idx'
		end
		@_tg._at(valueptr_at(idx))._set(v)
	end
	def push(v)
		self[length] = v
		self
	end
	def <<(v) ; push(v) ; end
	def pop
		l = length
		if l > 0
			v = self[l-1]
			delete_at(l-1)
		end
		v
	end

	include Enumerable
	def each ; (0...length).each { |i| yield self[i] } ; end
end
class StlVector16 < StlVector32
	def length
		DFHack.memory_vector16_length(@_memaddr)
	end
	def valueptr_at(idx)
		DFHack.memory_vector16_ptrat(@_memaddr, idx)
	end
	def insert_at(idx, val)
		DFHack.memory_vector16_insert(@_memaddr, idx, val)
	end
	def delete_at(idx)
		DFHack.memory_vector16_delete(@_memaddr, idx)
	end
end
class StlVector8 < StlVector32
	def length
		DFHack.memory_vector8_length(@_memaddr)
	end
	def valueptr_at(idx)
		DFHack.memory_vector8_ptrat(@_memaddr, idx)
	end
	def insert_at(idx, val)
		DFHack.memory_vector8_insert(@_memaddr, idx, val)
	end
	def delete_at(idx)
		DFHack.memory_vector8_delete(@_memaddr, idx)
	end
end
class StlBitVector < StlVector32
	def initialize ; end
	def length
		DFHack.memory_vectorbool_length(@_memaddr)
	end
	def insert_at(idx, val)
		DFHack.memory_vectorbool_insert(@_memaddr, idx, val)
	end
	def delete_at(idx)
		DFHack.memory_vectorbool_delete(@_memaddr, idx)
	end
	def [](idx)
		idx += length if idx < 0
		DFHack.memory_vectorbool_at(@_memaddr, idx) if idx >= 0 and idx < length
	end
	def []=(idx, v)
		idx += length if idx < 0
		if idx >= length
			insert_at(idx, v)
		elsif idx < 0
			raise 'invalid idx'
		else
			DFHack.memory_vectorbool_setat(@_memaddr, idx, v)
		end
	end
end
class StlString < MemStruct
	def _get
		DFHack.memory_read_stlstring(@_memaddr)
	end

	def _set(v)
		DFHack.memory_write_stlstring(@_memaddr, v)
	end
end
class StlDeque < MemStruct
	attr_accessor :_tglen, :_tg
	def initialize(tglen, tg)
		@_tglen = tglen
		@_tg = tg
	end
	# TODO
end

class DfFlagarray < MemStruct
	# TODO
end
class DfArray < Compound
	attr_accessor :_tglen, :_tg
	def initialize(tglen, tg)
		@_tglen = tglen
		@_tg = tg
	end

	field(:_length, 0) { number 32, false }
	field(:_ptr, 4) { number 32, false }

	def length ; _length ; end
	def size ; _length ; end
	def _tgat(i)
		@_tg._at(_ptr + i*@_tglen) if i >= 0 and i < _length
	end
	def [](i)
		i += _length if i < 0
		_tgat(i)._get
	end
	def []=(i, v)
		i += _length if i < 0
		_tgat(i)._set(v)
	end
	def _set(a)
		a.each_with_index { |v, i| self[i] = v }
	end

	include Enumerable
	def each ; (0...length).each { |i| yield self[i] } ; end
end
class DfLinkedList < MemStruct
	attr_accessor :_tg
	def initialize(tg)
		@_tg = tg
	end
end

class Global < MemStruct
	attr_accessor :_glob
	def initialize(glob)
		@_glob = glob
	end
	def _at(addr)
		g = DFHack.const_get(@_glob)
		g = DFHack.rtti_getclassat(g, addr)
		g.new._at(addr)
	end
end
end	# module MemHack


# cpp rtti name -> rb class
@rtti_n2c = {}
@rtti_c2n = {}

# vtableptr -> cpp rtti name (cache)
@rtti_n2v = {}
@rtti_v2n = {}

def self.rtti_n2c ; @rtti_n2c ; end
def self.rtti_c2n ; @rtti_c2n ; end

# register a ruby class with a cpp rtti class name
def self.rtti_register(cname, cls)
	@rtti_n2c[cname] = cls
	@rtti_c2n[cls] = cname
end

# return the ruby class to use for the cpp object at address if rtti info is available
def self.rtti_getclassat(cls, addr)
	if addr != 0 and @rtti_c2n[cls]
		# rtti info exist for class => cpp object has a vtable
		@rtti_n2c[rtti_readclassname(get_vtable_ptr(addr))] || cls
	else
		cls
	end
end

# try to read the rtti classname from an object vtable pointer
def self.rtti_readclassname(vptr)
	unless n = @rtti_v2n[vptr]
		n = @rtti_v2n[vptr] = get_rtti_classname(vptr)
		@rtti_n2v[n] = vptr
	end
	n
end

# return the vtable pointer from the cpp rtti name
def self.rtti_getvtable(cname)
	unless v = @rtti_n2v[cname]
		v = get_vtable(cname)
		@rtti_n2v[cname] = v
		@rtti_v2n[v] = cname if v != 0
	end
	v if v != 0
end

end


