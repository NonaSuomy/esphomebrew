#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// Map indexing stays native, but its 7 MB field array need not fit in one
// PSRAM block. Each page is under 64 KB for the current CMapField layout.
template<class T> class PaxPagedFields {
	static constexpr size_t perPage=512, maxPages=512;
	T *pages[maxPages]{};
	size_t length{}, allocated{};
	mutable size_t recent{};
public:
	PaxPagedFields()=default;
	PaxPagedFields(const PaxPagedFields &other) { *this=other; }
	PaxPagedFields &operator=(const PaxPagedFields &other) {
		if (this!=&other) {
			resize(other.size());
			for (size_t i=0;i<length;++i) (*this)[i]=other[i];
		}
		return *this;
	}
	~PaxPagedFields() { clear(); shrink_to_fit(); }
	size_t size() const { return length; }
	size_t capacity() const { return allocated*perPage; }
	bool empty() const { return length==0; }
	T &operator[](size_t i) { return pages[i/perPage][i%perPage]; }
	const T &operator[](size_t i) const { return pages[i/perPage][i%perPage]; }
	bool reserve(size_t count) {
		if (count>maxPages*perPage) return false;
		const size_t wanted=(count+perPage-1)/perPage;
		if (wanted>maxPages) return false;
		const size_t before=allocated;
		while (allocated<wanted) {
			void *block=std::malloc(sizeof(T)*perPage);
			if (!block) {
				while (allocated>before) { std::free(pages[--allocated]); pages[allocated]=nullptr; }
				return false;
			}
			pages[allocated++]=static_cast<T *>(block);
		}
		return true;
	}
	void resize(size_t count) {
		if (!reserve(count)) std::abort();
		while (length>count) (*this)[--length].~T();
		while (length<count) { new (&(*this)[length]) T(); ++length; }
	}
	void clear() { resize(0); }
	void shrink_to_fit() {
		const size_t wanted=(length+perPage-1)/perPage;
		while (allocated>wanted) { std::free(pages[--allocated]); pages[allocated]=nullptr; }
		recent=0;
	}
	size_t index_of(const T &field) const {
		const uintptr_t address=reinterpret_cast<uintptr_t>(&field);
		auto inPage=[&](size_t page) {
			const uintptr_t base=reinterpret_cast<uintptr_t>(pages[page]);
			return address>=base && address-base<sizeof(T)*perPage;
		};
		// Fog/reveal traversals typically stay in the same page for many calls.
		if (recent<allocated && inPage(recent))
			return recent*perPage+(address-reinterpret_cast<uintptr_t>(pages[recent]))/sizeof(T);
		for (size_t i=0;i<allocated;++i) if (inPage(i)) {
			recent=i; return i*perPage+(address-reinterpret_cast<uintptr_t>(pages[i]))/sizeof(T);
		}
		std::abort();
	}
};
