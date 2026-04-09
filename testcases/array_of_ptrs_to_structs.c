struct Item {
  int v;
  int w;
};

int main() {
  struct Item a, b, c;
  struct Item *arrp[3];
  struct Item *picked;

  a.v = 1; a.w = 2;
  b.v = 3; b.w = 4;
  c.v = 5; c.w = 6;

  arrp[0] = &a;
  arrp[1] = &b;
  arrp[2] = &c;

  picked = arrp[1];
  picked->w = picked->v + arrp[2]->v;
  return picked->w;
}
