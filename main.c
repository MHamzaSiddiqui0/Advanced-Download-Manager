// make with gcc -o main main.c -lcurl
#include<stdio.h>
#include<curl/curl.h>
#include<stdlib.h>
#include<string.h>

size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    // printf("Written: %zu bytes\n", written); // Debugging: Print bytes written
    return written;
}

int main(){
    CURL *curlpointer;
    CURLcode response; // Response of curl request
    FILE *fp;
    const char *fileurl = "https://filesamples.com/samples/document/doc/sample1.doc";
    const char *output_filename = "filename.doc"; 
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curlpointer = curl_easy_init();
    if(curlpointer){ // no error
        fp = fopen(output_filename,"wb");
        if(!fp){
            printf("error");
            return 1;
        }
        
        // Use set opt to set options for file download https://curl.se/libcurl/c/curl_easy_setopt.html
        // set url
        curl_easy_setopt(curlpointer,CURLOPT_URL,fileurl);
        // set write function
        curl_easy_setopt(curlpointer,CURLOPT_WRITEFUNCTION,write_data);
        // set file to be written in 
        curl_easy_setopt(curlpointer,CURLOPT_WRITEDATA,fp);

        response = curl_easy_perform(curlpointer); // perfroms curl request and saves response, returns CURLE_OK if no error
        
        if(response != CURLE_OK){
            printf("Error: %s",curl_easy_strerror(response));
        }
        else{
            printf("File downloaded");
        }
        fclose(fp);
        curl_easy_cleanup(curlpointer); // thread cleanup
    }
    else{
            printf("error");
        }
    curl_global_cleanup();
    return 0;
}
