; Inject JavaScript into script elements
((element
  (start_tag
    (tag_name) @_tag_name)
  (raw_text) @injection.content)
  (#match? @_tag_name "^[Ss][Cc][Rr][Ii][Pp][Tt]$")
  (#set! injection.language "javascript"))

; Inject CSS into style elements
((element
  (start_tag
    (tag_name) @_tag_name)
  (raw_text) @injection.content)
  (#match? @_tag_name "^[Ss][Tt][Yy][Ll][Ee]$")
  (#set! injection.language "css"))
