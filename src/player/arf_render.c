/** @file arf_render.c
 *  @brief Implementation of the renderer described in arf_render.h.
 *
 *  The body shaders are the ones the producing pipeline uses, loaded unchanged
 *  from disk.  They expect a background texture to sample for a screen-space
 *  pseudo-reflection; a standalone player has no camera image behind the
 *  avatar, so a one pixel texture is bound and the reflection is left at zero
 *  strength, which is exactly the matte look the pipeline shows at --shiny 0.
 *
 *  @author Ammar Qammaz (AmmarkoV)
 */

#include "arf_render.h"

#include <GL/glew.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct arfRenderer
{
    const struct arfAvatar *avatar;

    GLuint meshProgram;
    GLuint vertexArray;
    GLuint positionBuffer;
    GLuint normalBuffer;
    GLuint indexBuffer;
    GLuint sceneTexture;

    GLint  uniformModelViewProjection;
    GLint  uniformView;
    GLint  uniformColor;
    GLint  uniformScene;
    GLint  uniformResolution;
    GLint  uniformShiny;
    GLint  uniformAlpha;

    GLuint rectProgram;
    GLuint rectVertexArray;
    GLuint rectBuffer;
    GLint  uniformRectColor;

    unsigned int indexCount;
};

static const char arfRectVertexShader[] =
    "#version 330 core\n"
    "layout(location=0) in vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos,0.0,1.0); }\n";

static const char arfRectFragmentShader[] =
    "#version 330 core\n"
    "uniform vec4 uColor;\n"
    "out vec4 fragColor;\n"
    "void main() { fragColor = uColor; }\n";

/** @brief Read a whole text file into a NUL terminated heap block. */
static char *arfReadTextFile(const char *path)
{
    FILE *file = fopen(path,"rb");
    if (file==0) { return 0; }

    fseek(file,0,SEEK_END);
    long size = ftell(file);
    fseek(file,0,SEEK_SET);

    if (size < 0) { fclose(file); return 0; }

    char *text = (char *) malloc((size_t) size + 1);
    if (text==0) { fclose(file); return 0; }

    if (fread(text,1,(size_t) size,file) != (size_t) size)
    {
        free(text);
        fclose(file);
        return 0;
    }

    text[size] = 0;
    fclose(file);
    return text;
}

static GLuint arfCompileShader(GLenum type, const char *source, const char *label)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader,1,&source,0);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader,GL_COMPILE_STATUS,&compiled);

    if (compiled != GL_TRUE)
    {
        char log[2048];
        GLsizei length = 0;
        glGetShaderInfoLog(shader,sizeof(log)-1,&length,log);
        log[length] = 0;
        fprintf(stderr,"failed to compile %s:\n%s\n",label,log);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static GLuint arfLinkProgram(const char *vertexSource, const char *fragmentSource, const char *label)
{
    GLuint vertexShader   = arfCompileShader(GL_VERTEX_SHADER,vertexSource,label);
    GLuint fragmentShader = arfCompileShader(GL_FRAGMENT_SHADER,fragmentSource,label);

    if ( (vertexShader==0) || (fragmentShader==0) )
    {
        if (vertexShader!=0)   { glDeleteShader(vertexShader);   }
        if (fragmentShader!=0) { glDeleteShader(fragmentShader); }
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program,vertexShader);
    glAttachShader(program,fragmentShader);
    glLinkProgram(program);

    /* The shaders are only referenced by the program from here on. */
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);

    GLint linked = GL_FALSE;
    glGetProgramiv(program,GL_LINK_STATUS,&linked);

    if (linked != GL_TRUE)
    {
        char log[2048];
        GLsizei length = 0;
        glGetProgramInfoLog(program,sizeof(log)-1,&length,log);
        log[length] = 0;
        fprintf(stderr,"failed to link %s:\n%s\n",label,log);
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

struct arfRenderer *arfRendererCreate(const struct arfAvatar *avatar, const char *shaderDirectory)
{
    char vertexPath[1024];
    char fragmentPath[1024];

    snprintf(vertexPath,sizeof(vertexPath),"%s/default.vert",shaderDirectory);
    snprintf(fragmentPath,sizeof(fragmentPath),"%s/default.frag",shaderDirectory);

    char *vertexSource   = arfReadTextFile(vertexPath);
    char *fragmentSource = arfReadTextFile(fragmentPath);

    if ( (vertexSource==0) || (fragmentSource==0) )
    {
        fprintf(stderr,"cannot read the body shaders from \"%s\" -- pass --shaders <dir>\n",shaderDirectory);
        free(vertexSource);
        free(fragmentSource);
        return 0;
    }

    struct arfRenderer *renderer = (struct arfRenderer *) calloc(1,sizeof(struct arfRenderer));
    if (renderer==0)
    {
        free(vertexSource);
        free(fragmentSource);
        return 0;
    }

    renderer->avatar     = avatar;
    renderer->indexCount = avatar->mesh.numberOfTriangles * 3;

    renderer->meshProgram = arfLinkProgram(vertexSource,fragmentSource,"the body shaders");
    free(vertexSource);
    free(fragmentSource);

    renderer->rectProgram = arfLinkProgram(arfRectVertexShader,arfRectFragmentShader,"the overlay shaders");

    if ( (renderer->meshProgram==0) || (renderer->rectProgram==0) )
    {
        arfRendererDestroy(renderer);
        return 0;
    }

    renderer->uniformModelViewProjection = glGetUniformLocation(renderer->meshProgram,"uMVP");
    renderer->uniformView                = glGetUniformLocation(renderer->meshProgram,"uView");
    renderer->uniformColor               = glGetUniformLocation(renderer->meshProgram,"uColor");
    renderer->uniformScene               = glGetUniformLocation(renderer->meshProgram,"uScene");
    renderer->uniformResolution          = glGetUniformLocation(renderer->meshProgram,"uResolution");
    renderer->uniformShiny               = glGetUniformLocation(renderer->meshProgram,"uShiny");
    renderer->uniformAlpha               = glGetUniformLocation(renderer->meshProgram,"uAlpha");
    renderer->uniformRectColor           = glGetUniformLocation(renderer->rectProgram,"uColor");

    size_t vertexBytes = (size_t) avatar->mesh.numberOfVertices * 3 * sizeof(float);

    glGenVertexArrays(1,&renderer->vertexArray);
    glBindVertexArray(renderer->vertexArray);

    /* Positions and normals are rewritten every frame after CPU skinning;
     * the topology never changes. */
    glGenBuffers(1,&renderer->positionBuffer);
    glBindBuffer(GL_ARRAY_BUFFER,renderer->positionBuffer);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr) vertexBytes,avatar->mesh.positions,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void *) 0);

    glGenBuffers(1,&renderer->normalBuffer);
    glBindBuffer(GL_ARRAY_BUFFER,renderer->normalBuffer);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr) vertexBytes,0,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,3*sizeof(float),(void *) 0);

    glGenBuffers(1,&renderer->indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,renderer->indexBuffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t) renderer->indexCount * sizeof(unsigned int)),
                 avatar->mesh.indices,GL_STATIC_DRAW);

    glBindVertexArray(0);

    /* One opaque black pixel standing in for the pipeline's camera image. */
    const unsigned char blackPixel[4] = { 0, 0, 0, 255 };
    glGenTextures(1,&renderer->sceneTexture);
    glBindTexture(GL_TEXTURE_2D,renderer->sceneTexture);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,blackPixel);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D,0);

    glGenVertexArrays(1,&renderer->rectVertexArray);
    glBindVertexArray(renderer->rectVertexArray);
    glGenBuffers(1,&renderer->rectBuffer);
    glBindBuffer(GL_ARRAY_BUFFER,renderer->rectBuffer);
    glBufferData(GL_ARRAY_BUFFER,8*sizeof(float),0,GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,2*sizeof(float),(void *) 0);
    glBindVertexArray(0);

    return renderer;
}

void arfRendererDestroy(struct arfRenderer *renderer)
{
    if (renderer==0) { return; }

    if (renderer->meshProgram!=0)     { glDeleteProgram(renderer->meshProgram);          }
    if (renderer->rectProgram!=0)     { glDeleteProgram(renderer->rectProgram);          }
    if (renderer->positionBuffer!=0)  { glDeleteBuffers(1,&renderer->positionBuffer);    }
    if (renderer->normalBuffer!=0)    { glDeleteBuffers(1,&renderer->normalBuffer);      }
    if (renderer->indexBuffer!=0)     { glDeleteBuffers(1,&renderer->indexBuffer);       }
    if (renderer->rectBuffer!=0)      { glDeleteBuffers(1,&renderer->rectBuffer);        }
    if (renderer->vertexArray!=0)     { glDeleteVertexArrays(1,&renderer->vertexArray);  }
    if (renderer->rectVertexArray!=0) { glDeleteVertexArrays(1,&renderer->rectVertexArray); }
    if (renderer->sceneTexture!=0)    { glDeleteTextures(1,&renderer->sceneTexture);     }

    free(renderer);
}

void arfRendererBeginFrame(int width, int height)
{
    glViewport(0,0,width,height);
    glClearColor(0.09f,0.10f,0.12f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
}

void arfRendererUpload(struct arfRenderer *renderer, const struct arfPose *pose)
{
    if ( (renderer==0) || (pose==0) ) { return; }

    GLsizeiptr bytes = (GLsizeiptr)((size_t) pose->numberOfVertices * 3 * sizeof(float));

    glBindBuffer(GL_ARRAY_BUFFER,renderer->positionBuffer);
    glBufferSubData(GL_ARRAY_BUFFER,0,bytes,pose->positions);

    glBindBuffer(GL_ARRAY_BUFFER,renderer->normalBuffer);
    glBufferSubData(GL_ARRAY_BUFFER,0,bytes,pose->normals);

    glBindBuffer(GL_ARRAY_BUFFER,0);
}

void arfRendererDrawMesh(struct arfRenderer *renderer, const float *modelViewProjection,
                         const float *view, const float *color)
{
    if (renderer==0) { return; }

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT,viewport);

    glUseProgram(renderer->meshProgram);

    /* GL_TRUE because every matrix in this player is row-major, matching the
     * container's own convention -- see arf.h. */
    glUniformMatrix4fv(renderer->uniformModelViewProjection,1,GL_TRUE,modelViewProjection);
    glUniformMatrix4fv(renderer->uniformView,1,GL_TRUE,view);
    glUniform3fv(renderer->uniformColor,1,color);
    glUniform2f(renderer->uniformResolution,(float) viewport[2],(float) viewport[3]);
    glUniform1f(renderer->uniformShiny,0.0f);
    glUniform1f(renderer->uniformAlpha,1.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,renderer->sceneTexture);
    glUniform1i(renderer->uniformScene,0);

    glBindVertexArray(renderer->vertexArray);
    glDrawElements(GL_TRIANGLES,(GLsizei) renderer->indexCount,GL_UNSIGNED_INT,0);
    glBindVertexArray(0);

    glUseProgram(0);
}

void arfRendererDrawRect(struct arfRenderer *renderer, float x0, float y0, float x1, float y1,
                         float red, float green, float blue, float alpha)
{
    if (renderer==0) { return; }

    float corners[8] =
    {
        x0*2.0f-1.0f, y0*2.0f-1.0f,
        x1*2.0f-1.0f, y0*2.0f-1.0f,
        x0*2.0f-1.0f, y1*2.0f-1.0f,
        x1*2.0f-1.0f, y1*2.0f-1.0f
    };

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    glUseProgram(renderer->rectProgram);
    glUniform4f(renderer->uniformRectColor,red,green,blue,alpha);

    glBindVertexArray(renderer->rectVertexArray);
    glBindBuffer(GL_ARRAY_BUFFER,renderer->rectBuffer);
    glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(corners),corners);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glBindVertexArray(0);

    glUseProgram(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
}
