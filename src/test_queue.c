#include <stdio.h>
#include "linkqueue.h"

int main(void)
{
	QNode_t *q = CreateLinkQueue();
	QDataType d, out;
	int i;

	for (i = 0; i < 100; i++)
	{
		d.data.id = i;
		EnterLinkQueue(q, d);
	}

	printf("count=%d drops=%d\n", GetQueueCount(q), GetDropCount());

	while (0 == PopLinkQueue(q, &out))
	{
		printf("pop id=%d\n", out.data.id);
	}

	DestroyLinkQueue(&q);
	return 0;
}
