struct Pair {
  int x;
  int y;
};

int main() {
  struct Pair arr[2];
  struct Pair *p = arr;

  p[1].y = 5;
  return p[1].y;
}
