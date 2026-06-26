#include "value.h"
#include "test_main.h"

using namespace coal;

void test_value() {
  CHECK(Value::nil().truthy() == false);
  CHECK(Value::integer(0).truthy() == false);
  CHECK(Value::integer(3).truthy() == true);
  CHECK(Value::boolean(false).truthy() == false);
  CHECK(Value::boolean(true).truthy() == true);

  CHECK(Value::number(2.0).is_number());
  CHECK(Value::integer(2).is_number());
  CHECK(Value::nil().is_number() == false);

  CHECK(Value::integer(42).tag == Tag::Int);
  CHECK(Value::integer(42).as.i == 42);
  CHECK(Value::number(1.5).as.d == 1.5);
}
