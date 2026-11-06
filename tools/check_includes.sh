#!/bin/bash
# Every file under src/ and test/ that names a standard type includes the
# header that declares it, instead of getting it through some other header.
# Leaning on a transitive include is what broke master when ubuntu-latest
# moved to 26.04 (beads-972k: std::sort without <algorithm>).
#
# Portable to the bash 3.2 macOS ships: no associative arrays (beads-44jk;
# `declare -A` made this script fail before it checked anything there).
#
# Exit status: the number of missing includes (0 = clean).

# "token header" pairs.  Extend with care: a token must not be a prefix of
# another type (std::map also matches std::multimap, which <map> declares).
pairs="std::string string
std::map map"

errors=0
while read -r token header; do
  [ -n "$token" ] || continue
  echo "Looking for ${token} and ${header}"
  # Files that name the type at all, as a whole word.
  uses="$(grep -rlIw -- "${token}" src/ test/ | sort -u)"
  for f in ${uses}; do
    # Exactly <header>: not <header_view>, not a mention in a comment.
    if ! grep -qE "^[[:space:]]*#[[:space:]]*include[[:space:]]*<${header}>" "$f"; then
      echo "${f} uses ${token} but doesn't include ${header}"
      errors=$((errors + 1))
    fi
  done
done <<EOF
${pairs}
EOF

exit ${errors}
