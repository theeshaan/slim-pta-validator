int main() {
  int *p;
  int a = 1;
  int b = 2;

  if (a > 0)
    p = &a;
  else
    p = &b;

  *p = 42;
  return a + b;
}
