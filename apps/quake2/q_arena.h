#pragma once
int Q_Arena_Init(int size);
void Q_Arena_Shutdown(void);
int Q_Arena_Reserved(void);
void *Q_Arena_Carve(int size);
void *Q_Arena_Alloc(int size);
int Q_Arena_AllocationSize(void *p);
void Q_Arena_Free(void *p);
int Q_Arena_Used(void);
int Q_Arena_FreeSize(void);
int Q_Arena_Largest(void);
int Q_Arena_Count(void);
int Q_Arena_Contains(void *p);
void *Hunk_AllocBlock(int size);
int Hunk_Contains(void *p);
void Hunk_Free(void *base);
