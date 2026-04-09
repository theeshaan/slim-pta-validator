static int read_value(int *p) {
  return *p;
}

int main() {
  int x = 7;
  return read_value(&x) - 7;
}
