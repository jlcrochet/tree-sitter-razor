#!/bin/env bash
for type in Full Short; do
  lower="${type,,}"

  ./triehash/triehash.pl \
    --enum-name="${type}CharacterReference" \
    --function-name="lookup_${lower}_character_reference" \
    --label-prefix="${type}CharacterReference_" \
    src/tables/"${lower}_character_references.txt" > "src/tables/${lower}_character_references.h"
done
