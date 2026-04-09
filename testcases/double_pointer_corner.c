int main() {
  int a = 0, b = 1;
  int *p;
  int **pp;

  p = &a;
  pp = &p;
  **pp = 7;

  p = &b;
  **pp = **pp + 3;

  return a + b;
}
