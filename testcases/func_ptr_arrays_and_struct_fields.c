typedef int (*Op)(int, int);

struct OpBox {
  Op op;
  int bias;
};

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }

int main() {
  Op ops[2];
  struct OpBox boxes[2];
  struct OpBox *bp;

  ops[0] = add;
  ops[1] = sub;

  boxes[0].op = ops[0];
  boxes[0].bias = 10;
  boxes[1].op = ops[1];
  boxes[1].bias = 5;

  bp = &boxes[0];
  return bp->op(7, 3) + boxes[1].op(7, 3) + bp->bias;
}
