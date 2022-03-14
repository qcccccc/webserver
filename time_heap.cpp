#include "time_heap.h"
#include <stdio.h>
#include <algorithm>
time_heap::time_heap(int cap) : capacity(cap), cur_size(0)
{
    array = new heap_timer *[capacity];
    if (!array)
    {
        fprintf(stderr, "init heap_timer failed\n");
        return;
    }
    for (int i = 0; i < capacity; ++i)
        array[i] = NULL;
}

time_heap ::time_heap(heap_timer **init_array, int size, int capacity)
    : capacity(capacity), cur_size(size)
{
    if (capacity < size)
    {
        fprintf(stderr, "init heap_timer 1 for init_array failed.\n");
        return;
    }

    //创建堆数组
    array = new heap_timer *[capacity];
    if (!array)
    {
        fprintf(stderr, "init heap_timer 2 failed.\n");
        return;
    }
    for (int i = 0; i < capacity; ++i)
        array[i] = NULL;
    if (size != 0)
    {
        for (int i = 0; i < size; ++i)
        {
            array[i] = init_array[i];
            array[i]->pos = i;
        }
        for (int i = (cur_size - 1) / 2; i >= 0; --i)
        {
            percolate_down(i);
        }
    }
}

time_heap::~time_heap()
{
    for (int i = 0; i < cur_size; ++i)
        delete array[i];

    delete[] array;
}

int time_heap::add_timer(heap_timer *timer)
{
    if (!timer)
        return -1;
    if (cur_size >= capacity)
        resize();
    int hole = cur_size++;
    int parent = 0;
    for (; hole > 0; hole = parent)
    {
        parent = (hole - 1) / 2;
        if (array[parent]->expire <= timer->expire)
            break;
        array[hole] = array[parent];
        array[hole]->pos =hole;
    }
    array[hole] = timer;
    array[hole]->pos=hole;
    return 0;
}
void time_heap::del_timer(heap_timer *timer)
{
    if (!timer)
        return;
    int element = timer->pos;
    int parent = 0;
    for (; element > 0; element = parent)
    {
        parent = (element - 1) / 2;
        array[element] = array[parent];
        array[element]->pos = element;
    }
    array[element] = timer;
    array[element]->pos = element;
    if (array[0] == timer)
        pop_timer();
    else
        throw std::exception();
}

void time_heap::adjust_timer(heap_timer *timer)
{
    percolate_down(timer->pos);
}

heap_timer *time_heap::top() const
{
    if (empty())
    {
        return NULL;
    }
    return array[0];
}

void time_heap::pop_timer()
{
    if (empty())
        return;
    if (array[0])
    {
        delete array[0];
        array[0] = array[--cur_size];
        array[0]->pos=0;
        percolate_down(0);
    }
}
void time_heap::tick()
{
    heap_timer *tmp = array[0];
    time_t cur = time(NULL);
    while (!empty())
    {
        if (!tmp)
            break;
        if (tmp->expire > cur)
            break;
        if (array[0]->cb_func)
            array[0]->cb_func(array[0]->user_data);
        pop_timer();
        tmp = array[0];
    }
}

void time_heap::percolate_down(int hole)
{
    int tmp = hole;
    int lchild = (hole * 2) + 1;
    int rchild = (hole * 2) + 2;

    if (lchild <= (cur_size - 1) && array[lchild]->expire < array[hole]->expire)
        tmp = hole * 2 + 1;
    if (rchild <= (cur_size - 1) && array[rchild]->expire < array[hole]->expire)
        tmp = hole * 2 + 2;
    if (tmp != hole)
    {
        std::swap(array[tmp], array[hole]);
        std::swap(array[tmp]->pos, array[hole]->pos);
        percolate_down(tmp);
    }
}

void time_heap::resize()
{
    heap_timer **tmp = new heap_timer *[2 * capacity];

    for (int i = 0; i < (2 * capacity); ++i)
        tmp[i] = NULL;

    if (!tmp)
    {
        fprintf(stderr, "resize() failed.\n");
        return;
    }

    capacity = 2 * capacity;

    for (int i = 0; i < cur_size; ++i)
    {
        tmp[i] = array[i];
        tmp[i]->pos = i;
    }
    delete[] array;
    array = tmp;
}
