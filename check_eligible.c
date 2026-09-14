#include <stdio.h>
#include <sys/prctl.h>

#ifndef PR_GET_IVH_ELIGIBLE
#define PR_GET_IVH_ELIGIBLE 80
#endif

int main(void)
{
	printf("PF_IVH_ELIGIBLE = %d\n", prctl(PR_GET_IVH_ELIGIBLE, 0, 0, 0, 0));
	return 0;
}
