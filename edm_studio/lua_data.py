"""Bounded reader for declarative Lua data. No Lua code is executed."""
from dataclasses import dataclass
import re


class LuaDataError(ValueError):
    pass


@dataclass
class Token:
    kind: str
    value: object
    line: int


def tokenize(text):
    if len(text) > 2_000_000:
        raise LuaDataError('description.lua exceeds 2 MB')
    out=[];i=0;line=1
    while i<len(text):
        c=text[i]
        if c.isspace():
            line+=c=='\n';i+=1;continue
        comment=text.startswith('--',i)
        start=i+2 if comment else i
        long=re.match(r'\[(=*)\[',text[start:])
        if long:
            begin=start+len(long[0]);end=text.find(']'+long[1]+']',begin)
            if end<0:raise LuaDataError(f'Unclosed long string/comment at line {line}')
            value=text[begin:end]
            if value.startswith('\r\n'):value=value[2:]
            elif value.startswith('\n'):value=value[1:]
            if not comment:out.append(Token('string',value,line))
            finish=end+len(long[1])+2;line+=text[i:finish].count('\n');i=finish;continue
        if comment:
            end=text.find('\n',i);i=len(text) if end<0 else end;continue
        if c in ('"',"'"):
            quote=c;value=[];first=line;i+=1
            while i<len(text) and text[i]!=quote:
                if text[i]!='\\':
                    value.append(text[i]);line+=text[i]=='\n';i+=1;continue
                i+=1
                if i>=len(text):raise LuaDataError(f'Unclosed string at line {first}')
                e=text[i];i+=1
                escapes={'n':'\n','r':'\r','t':'\t','a':'\a','b':'\b','f':'\f','v':'\v','\\':'\\','"':'"',"'":"'"}
                if e in escapes:value.append(escapes[e])
                elif e.isdigit():
                    digits=e
                    while i<len(text) and text[i].isdigit() and len(digits)<3:digits+=text[i];i+=1
                    number=int(digits)
                    if number>255:raise LuaDataError(f'Invalid decimal escape at line {first}')
                    value.append(chr(number))
                elif e=='x':
                    digits=text[i:i+2]
                    if not re.fullmatch('[0-9a-fA-F]{2}',digits):raise LuaDataError('Invalid hex escape')
                    value.append(chr(int(digits,16)));i+=2
                elif e=='z':
                    while i<len(text) and text[i].isspace():line+=text[i]=='\n';i+=1
                elif e in '\r\n':
                    if e=='\r' and i<len(text) and text[i]=='\n':i+=1
                    value.append('\n');line+=1
                else:raise LuaDataError(f'Unsupported escape \\{e} at line {first}')
            if i==len(text):raise LuaDataError(f'Unclosed string at line {first}')
            i+=1;out.append(Token('string',''.join(value),first));continue
        m=re.match(r'0[xX][0-9a-fA-F]+|(?:\d+(?:\.(?!\.)\d*)?|\.\d+)(?:[eE][+-]?\d+)?',text[i:])
        if m:
            raw=m[0];value=int(raw,16) if raw.lower().startswith('0x') else float(raw)
            out.append(Token('number',value,line));i+=len(raw);continue
        m=re.match(r'[A-Za-z_][A-Za-z_0-9]*',text[i:])
        if m:out.append(Token('name',m[0],line));i+=len(m[0]);continue
        if text.startswith('..',i):out.append(Token('..','..',line));i+=2;continue
        out.append(Token(c,c,line));i+=1
    out.append(Token('eof',None,line))
    if len(out)>150_000:raise LuaDataError('Too many Lua tokens')
    return out


class DataParser:
    def __init__(self,text):
        self.tokens=tokenize(text);self.i=0;self.depth=0;self.warnings=[]
        self.env={'DIFFUSE':0,'NORMAL_MAP':1,'SPECULAR':2,'DECAL':3,'ROUGHNESS_METALLIC':13}

    def peek(self,n=0):return self.tokens[min(self.i+n,len(self.tokens)-1)]
    def take(self,kind=None):
        t=self.peek()
        if kind and t.kind!=kind:raise LuaDataError(f'Expected {kind}, got {t.value!r} at line {t.line}')
        self.i+=1;return t

    def expression(self):
        value=self.atom()
        while self.peek().kind=='..':
            self.take();right=self.atom()
            if not isinstance(value,(str,int,float)) or not isinstance(right,(str,int,float)):raise LuaDataError('Invalid concatenation')
            value=str(value)+str(right)
        return value

    def atom(self):
        t=self.take()
        if t.kind in ('string','number'):return t.value
        if t.kind=='-':
            value=self.atom()
            if not isinstance(value,(int,float)):raise LuaDataError('Expected number after minus')
            return -value
        if t.kind=='(':value=self.expression();self.take(')');return value
        if t.kind=='{':
            self.depth+=1
            if self.depth>40:raise LuaDataError('Lua table is too deeply nested')
            result={};index=1
            while self.peek().kind!='}':
                if self.peek().kind=='[':
                    self.take();key=self.expression();self.take(']');self.take('=');value=self.expression()
                elif self.peek().kind=='name' and self.peek(1).kind=='=':
                    key=self.take().value;self.take();value=self.expression()
                else:key=index;index+=1;value=self.expression()
                if not isinstance(key,(str,int,float,bool)):raise LuaDataError('Invalid Lua table key')
                result[key]=value
                if self.peek().kind in (',',';'):self.take()
                elif self.peek().kind!='}':raise LuaDataError(f'Expected table separator at line {self.peek().line}')
            self.take('}');self.depth-=1;return result
        if t.kind=='name':
            if t.value in ('true','false','nil'):return {'true':True,'false':False,'nil':None}[t.value]
            if t.value=='_' and self.peek().kind=='(':
                self.take();value=self.expression();self.take(')');return value
            if t.value not in self.env:raise LuaDataError(f'Unknown expression {t.value!r} at line {t.line}')
            value=self.env[t.value]
            while self.peek().kind=='[':
                self.take();key=self.expression();self.take(']');value=value[key]
            return value
        raise LuaDataError(f'Unsupported expression {t.value!r} at line {t.line}')

    def parse(self):
        while self.peek().kind!='eof':
            begin=self.i;t=self.peek()
            if t.kind==';':self.take();continue
            if t.value in ('if','for','while','repeat','function','do'):
                # Never reinterpret assignments inside executable control flow as unconditional data.
                self.warnings.append(f'第 {t.line} 行包含动态 Lua 控制流程，后续语句未解析。')
                break
            if t.value=='local':self.take()
            try:
                name=self.take('name').value;keys=[]
                while self.peek().kind=='[':
                    self.take();keys.append(self.expression());self.take(']')
                self.take('=');value=self.expression()
                if keys:
                    target=self.env[name]
                    for key in keys[:-1]:target=target[key]
                    target[keys[-1]]=value
                else:self.env[name]=value
            except (LuaDataError,KeyError,TypeError) as error:
                if t.value in ('livery','custom_args') or (t.value=='local' and self.tokens[begin+1].value=='livery'):
                    raise LuaDataError(str(error)) from error
                self.warnings.append(f'第 {t.line} 行未解析：{str(error)[:140]}')
                # Skip this whole line, rather than invoking dofile/require or arbitrary calls.
                self.i=max(self.i,begin+1)
                while self.peek().kind!='eof' and self.peek().line==t.line:self.i+=1
        return self.env,self.warnings


def read_data(text):
    return DataParser(text.replace('\r\n','\n').replace('\r','\n')).parse()
