int main() {
  int a = 1;
  int b = 2;
  int *p;

  p = &a;
  *p = 7;

  p = &b;
  return *p;
}
