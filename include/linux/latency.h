#ifndef __LATENCY_H__
#define __LATENCY_H__
enum lat_entry {
	PREP1 = 0,
	PREP_START = PREP1,
	PREP2 = 1,
	PREP3 = 2,
	PREP4 = 3,
	PREP_END = PREP4,
	COMP1 = PREP_END + 1,
	COMP2 = COMP1 + 1,
	SUB = COMP2 + 1,
	SCHED = SUB + 1,
	MAX_LAT_ENTRIES = SCHED + 1,
};

struct latency {
	u64 start[MAX_LAT_ENTRIES];
	u64 end[MAX_LAT_ENTRIES];
};

static inline bool zero_lat(struct latency *lat, enum lat_entry entry)
{
	if (lat->start[entry] == U64_MAX && lat->end[entry] == 0)
		return true;
	return false;
}

static inline u64 get_lat(struct latency *lat, enum lat_entry entry)
{
	return zero_lat(lat, entry)? 0: lat->end[entry] - lat->start[entry];
}

static inline u64 prep_latency_sum(struct latency *lat)
{
	u64 sum = 0;
	int i;

	for (i = PREP_START; i <= PREP_END; i++)
		sum += get_lat(lat, i);

	return sum;

}

static inline u64 latency_sum(struct latency *lat, bool sched)
{
	u64 sum = 0;
	int i;

	for (i = PREP_START; i < MAX_LAT_ENTRIES; i++)
		sum += get_lat(lat, i);

	if (sched)
		sum += get_lat(lat, SCHED);

	return sum;
}

static inline void latency_init(struct latency *lat)
{
	int i;

	for (i = 0; i < MAX_LAT_ENTRIES; i++) {
		lat->start[i] = U64_MAX;
		lat->end[i] = 0;
	}
}

static __maybe_unused void latency_minmax(struct latency *lhs, struct latency *rhs)
{
	int i;

	for (i = 0; i < MAX_LAT_ENTRIES; i++) {
		lhs->start[i] = min(lhs->start[i], rhs->start[i]);
		lhs->end[i] = max(lhs->end[i], rhs->end[i]);
	}
}
#endif
