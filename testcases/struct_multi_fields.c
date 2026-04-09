struct Node {
  int a;
  int b;
  struct Node *next;
};

int main() {
  struct Node n1, n2;
  struct Node *p, *q;

  n1.a = 1; n1.b = 2; n1.next = &n2;
  n2.a = 3; n2.b = 4; n2.next = 0;

  p = &n1;
  q = p->next;
  p->b = q->a + 10;

  return p->b;
}
