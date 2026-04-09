int main() {
  int a = 1, b = 2;
  int *p;
  int i;

  p = &a;
  for (i = 0; i < 4; i++) {
    if (i & 1)
      p = &b;
    else
      p = &a;
    *p = *p + i;
  }

  return a + b;
}
