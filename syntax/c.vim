" highlight Functions
syn match cFunctions "\<[a-zA-Z_][a-zA-Z_0-9]*\>[^()]*)("me=e-2
syn match cFunctions "\<[a-zA-Z_][a-zA-Z_0-9]*\>\s*("me=e-1
hi cFunctions gui=NONE cterm=bold  ctermfg=blue")"")""

" comment
hi Comment ctermfg=10

" constant
hi Constant ctermfg=9

" for/if/while
" hi Statement ctermfg=27 cterm=bold
