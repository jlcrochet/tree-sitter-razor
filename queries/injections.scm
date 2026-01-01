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

; Inject Razor into text literal content (@:)
; This allows HTML elements, Razor expressions, etc. to be parsed within text literals
((razor_text_literal
  (text_literal_content) @injection.content)
  (#set! injection.language "razor")
  (#set! injection.include-children))
