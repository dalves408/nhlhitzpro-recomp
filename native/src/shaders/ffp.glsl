// Fixed-function body ported from the verified Windows shader.
  vec4 R[13u], O[13u];
   for (uint j=0u; j<13u; ++j) { R[j]=vec4(0u); O[j]=vec4(0u); }
  uvec4 opts=floatBitsToUint(K(9u)), slots=floatBitsToUint(K(75u));
  vec4 p=fetch(0u,vi), vc=fetch(slots.x,vi), sc=fetch(4u,vi);
  precise vec4 cp;
   for(uint row=0u;row<4u;++row) {
    precise vec4 products=K(row)*p;
    precise float sum=products.x+products.y; sum=sum+products.z; cp[row]=sum+products.w;
  }
  precise vec3 projected=cp.xyz/cp.w;
  precise vec3 screen=projected+K(74u).xyz;
  R[12u]=vec4(screen,cp.w);
  vec4 color=slots.z!=0u ? vc : vec4(1u,1u,1u,1u);
  if (opts.z!=0u) {
    vec3 nr=fetch(2u,vi).xyz;
    precise vec3 eye=vec3(dot(K(4u).xyz,p.xyz)+K(4u).w,dot(K(5u).xyz,p.xyz)+K(5u).w,dot(K(6u).xyz,p.xyz)+K(6u).w);
    precise vec3 normal=vec3(dot(K(4u).xyz,nr),dot(K(5u).xyz,nr),dot(K(6u).xyz,nr));
    float nl=length(normal); if(nl>1e-8) normal/=nl;
    if(opts.w!=0u && dot(normal,eye)>0u) normal=-normal;
    uint es=opts.y&3u, am=(opts.y>>2u)&3u, ds=(opts.y>>4u)&3u;
    vec3 ec=es==1u ? vc.xyz : sc.xyz, ac=am==1u ? vc.xyz : sc.xyz;
    vec3 em=K(8u).xyz;
    if(es==0u || am==0u) { color.xyz=K(7u).xyz; if(es!=0u || am!=0u) color.xyz+=(es!=0u?ec:ac)*em; }
    else color.xyz=ec+ac*em;
    color.w=ds==1u ? vc.w : ds==2u ? sc.w : K(7u).w;
     for(uint i=0u;i<8u;++i) {
      uint kind=(opts.x>>(2u*i))&3u, b=10u+i*8u; if(kind==0u) continue;
      vec3 light; float att=1.0;
      if(kind==1u) light=vec3(K(b+3u).y,K(b+3u).z,K(b+3u).w);
      else {
        light=vec3(K(b+5u).w,K(b+6u).x,K(b+6u).y)-eye;
        float dist=length(light), range=K(b+2u).y;
        if(dist>range && range>0u) continue; if(dist>1e-8) light/=dist;
        att=K(b+6u).z+K(b+6u).w*dist+K(b+7u).x*dist*dist; att=att>1e-8?1.0/att:1.0;
      }
      float ndl=max(0.0,dot(normal,light));
      vec3 ambient=K(b).xyz, diffuse=vec3(K(b).w,K(b+1u).x,K(b+1u).y);
      if(slots.z!=0u && am==1u) ambient*=vc.xyz; if(am==2u) ambient*=sc.xyz;
      if(slots.z!=0u && ds==1u) diffuse*=vc.xyz; if(ds==2u) diffuse*=sc.xyz;
      color.xyz+=att*(ambient+diffuse*ndl);
    }
  }
  O[3u]=saturate(color);
  O[9u]=fetch(slots.y,vi); O[10u]=fetch(10u,vi); O[11u]=fetch(11u,vi); O[12u]=fetch(12u,vi);
