struct Pair {
  int x;
  int y;
};

int main() {
  struct Pair arr[3];
  struct Pair *p;
  int i;

  for (i = 0; i < 3; i++) {
    arr[i].x = i;
    arr[i].y = i * 10;
  }

  p = &arr[1];
  p->x = p->y + arr[2].y;
  return p->x;
}
