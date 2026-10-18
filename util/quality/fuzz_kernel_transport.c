#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "9ring.h"
#include "virtio9p_core.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	P9TRing ring;
	P9Slot slot, out;
	u64 model[P9_RING_SIZE];
	u32 offered, limit;
	size_t offset;
	int front, count, result;

	memset(&ring, 0, sizeof(ring));
	memset(&slot, 0, sizeof(slot));
	front = 0;
	count = 0;
	for(offset = 0; offset < size; offset++){
		if(data[offset] & 1){
			slot.offset = offset < size ? data[offset] : 0;
			result = p9_tring_submit(&ring, &slot);
			if((result == 0) != (count < P9_RING_SIZE))
				__builtin_trap();
			if(result == 0){
				model[(front + count) & P9_RING_MASK] = slot.offset;
				count++;
			}
		}else{
			result = p9_tring_consume(&ring, &out);
			if((result == 1) != (count > 0))
				__builtin_trap();
			if(result == 1){
				if(out.offset != model[front])
					__builtin_trap();
				front = (front + 1) & P9_RING_MASK;
				count--;
			}
		}
		if((u32)(ring.head - ring.tail) != (u32)count)
			__builtin_trap();
	}
	if(size >= 8){
		offered = (u32)data[0] | (u32)data[1] << 8 |
			(u32)data[2] << 16 | (u32)data[3] << 24;
		limit = (u32)data[4] | (u32)data[5] << 8 |
			(u32)data[6] << 16 | (u32)data[7] << 24;
		(void)v9pqueuesize(offered, limit);
	}
	if(size <= UINT32_MAX)
		(void)v9pmessagevalid(data, size, size);
	if(size >= 6)
		(void)v9pchain2valid(data[0], data[1], data[2], data[3],
			v9pqueuesize(data[4], data[5]));
	return 0;
}
